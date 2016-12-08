﻿# ==============================================================================
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE.md file in the project root
# for full license information.
# ==============================================================================

import os
import math
from cntk.blocks import *  # non-layer like building blocks such as LSTM()
from cntk.layers import *  # layer-like stuff such as Linear()
from cntk.models import *  # higher abstraction level, e.g. entire standard models and also orsisrators like Sequential()
from cntk.utils import *
from cntk.io import MinibatchSource, CTFDeserializer, StreamDef, StreamDefs, INFINITELY_REPEAT, FULL_DATA_SWEEP
from cntk import Trainer, Evaluator
from cntk.learner import adam_sgd, learning_rate_schedule, momentum_as_time_constant_schedule, UnitType
from cntk.ops import cross_entropy_with_softmax, classification_error, splice, relu
from cntk.trainer import create_trainer

########################
# variables and stuff  #
########################

cntk_dir = os.path.dirname(os.path.abspath(__file__)) + "/../../../.."  # data resides in the CNTK folder
data_dir = cntk_dir + "/Examples/LanguageUnderstanding/ATIS/Data"       # under Examples/LanguageUnderstanding/ATIS
vocab_size = 943 ; num_labels = 129 ; num_intents = 26    # number of words in vocab, slot labels, and intent labels

model_dir = "./Models"

# model dimensions
emb_dim    = 150
hidden_dim = 300

########################
# define the reader    #
########################

def create_reader(path, is_training):
    return MinibatchSource(CTFDeserializer(path, StreamDefs(
        query         = StreamDef(field='S0', shape=vocab_size,  is_sparse=True),
        intent_labels = StreamDef(field='S1', shape=num_intents, is_sparse=True),  # (used for intent classification variant)
        slot_labels   = StreamDef(field='S2', shape=num_labels,  is_sparse=True)
    )), randomize=is_training, epoch_size = INFINITELY_REPEAT if is_training else FULL_DATA_SWEEP)

########################
# define the model     #
########################

def create_model_function():
  softplus = Function(lambda x: log (1 + exp (x)))

  softmux = Function(lambda sel, a, b: a)   # sel * a + (1-sel) * b)
  rnn = RNNUnit(hidden_dim, activation=relu)
  #gate = Dense(hidden_dim, activation=sigmoid)
  #pr_rnn = Function(lambda x, h: softmux(gate(x), x, rnn(x, h)))
  def pr_rnn_f(x, prev_h):
      r = rnn(x, prev_h)
      return r + x
      #selx = Dense(hidden_dim)(x)
      #selh = Dense(hidden_dim, bias=False)(prev_h)
      #sel = sigmoid (selx + selh)
      #return (1-sel) * r + sel * x
      #return sel * r + (1-sel) * x
      #return softmux(sel, r, x) #sel * r + (1-sel) * x
  pr_rnn = Function(pr_rnn_f)

  from cntk.ops.sequence import last
  with default_options(initial_state=0.1):  # inject an option to mimic the BS version identically; remove some day
    return Sequential([
        Embedding(emb_dim),
        Recurrence(LSTM(hidden_dim), go_backwards=False),
        #Recurrence(GRU(hidden_dim), go_backwards=False),
        #Recurrence(GRU(hidden_dim, activation=relu), go_backwards=False),
        #Recurrence(RNNUnit(hidden_dim, activation=relu), go_backwards=False),
        #Recurrence(RNNUnit(hidden_dim, activation=softplus), go_backwards=False),
        #Recurrence(pr_rnn, go_backwards=False),
        #Recurrence(RNNUnit(hidden_dim, activation=relu) >> Dense(hidden_dim, activation=relu), go_backwards=False),
        Dense(num_labels)
        #last,
        #Dense(num_intents)
    ])

########################
# define the criteria  #
########################

# compose model function and criterion primitives into a criterion function
#  takes:   Function: features -> prediction
#  returns: Function: (features, labels) -> (loss, metric)
# This function is generic and could be a stock function create_ce_classification_criterion().
def create_criterion_function(model):
    def criterion(x, y):
        z = model(x)
        ce   = cross_entropy_with_softmax(z, y)
        errs = classification_error      (z, y)
        #return Record(loss=ce, metric=errs)
        # BUGBUG: parameters passed to Record are not ordered. This pattern is not correct.
        return Record(ce=ce, errs=errs)
    return Function(criterion)

# alternative way of doing it, e.g. for use with Beta2
def create_criterion_function1(model):
    x, y = Placeholders(2)
    z = model(x)
    ce   = cross_entropy_with_softmax(z, y)
    errs = classification_error      (z, y)
    return combine ([ce, errs]) # (features, labels) -> (loss, metric)

########################
# train action         #
########################

def train(reader, model, max_epochs):

    # declare the model's input dimension
    model.replace_placeholders({model.placeholders[0]: input_variable(vocab_size)})
    # BUGBUG: ^^ Trainer requires this, although the criterion roots are not part of this.

    # criterion: (model args, labels) -> (loss, metric)
    #   here  (query, slot_labels) -> (ce, errs)
    criterion = create_criterion_function(model)

    # TODO: num_intents --> get from reader?
    labels = reader.streams.slot_labels
    #labels = reader.streams.intent_labels

    # declare argument types
    criterion.set_signature(Input(vocab_size, is_sparse=False), Input(num_labels, is_sparse=True))
    #criterion.set_signature(Input(vocab_size, is_sparse=False), Input(num_intents, is_sparse=True, dynamic_axes=[Axis.default_batch_axis()]))

    # iteration parameters  --needed here because learner schedule needs it
    epoch_size = 36000
    minibatch_size = 70
    #epoch_size = 1000 ; max_epochs = 1 # uncomment for faster testing

    # SGD parameters
    learner = adam_sgd(criterion.parameters,
                       lr         = learning_rate_schedule([0.003]*2+[0.0015]*12+[0.0003], UnitType.sample, epoch_size),
                       #lr         = learning_rate_schedule(0, UnitType.sample),
                       momentum   = momentum_as_time_constant_schedule(minibatch_size / -math.log(0.9)),
                       low_memory = True,
                       gradient_clipping_threshold_per_sample = 15,
                       gradient_clipping_with_truncation = True)

    # trainer
    trainer = create_trainer(model, criterion, learner)

    # process minibatches and perform model training
    log_number_of_parameters(model) ; print()
    progress_printer = ProgressPrinter(freq=100, first=10, tag='Training') # more detailed logging
    #progress_printer = ProgressPrinter(tag='Training')

    t = 0
    for epoch in range(max_epochs):         # loop over epochs
        epoch_end = (epoch+1) * epoch_size
        while t < epoch_end:               # loop over minibatches on the epoch
            # BUGBUG? The change of minibatch_size parameter vv has no effect.
            data = reader.next_minibatch(min(minibatch_size, epoch_end-t))     # fetch minibatch
            trainer.train_minibatch_from_data(criterion, data[reader.streams.query], data[labels])  # update model with it
            t += data[labels].num_samples                  # count samples processed so far
            progress_printer.update_with_trainer(trainer, with_metric=True)  # log progress
        loss, metric, actual_samples = progress_printer.epoch_summary(with_metric=True)

    return loss, metric # return values from last epoch


########################
# eval action          #
########################

def evaluate(reader, model):
    criterion = create_criterion_function(model)
    #criterion.set_signature(None, variable_of_type(num_labels, is_sparse=True))
    criterion.set_signature(variable_of_type(vocab_size, is_sparse=False), variable_of_type(num_labels, is_sparse=True))

    # process minibatches and perform evaluation
    evaluator = Evaluator(model, criterion.outputs[0], criterion.outputs[1])
    #progress_printer = ProgressPrinter(freq=100, first=10, tag='Evaluation') # more detailed logging
    progress_printer = ProgressPrinter(tag='Evaluation')
    while True:
        minibatch_size = 1000
        data = reader.next_minibatch(minibatch_size) # fetch minibatch
        if not data:                                 # until we hit the end
            break
        metric = evaluator.test_minibatch({ criterion.arguments[0]: data[reader.streams.query], criterion.arguments[1]: data[reader.streams.slot_labels] })
        progress_printer.update(0, data[reader.streams.slot_labels].num_samples, metric) # log progress
    loss, metric, actual_samples = progress_printer.epoch_summary(with_metric=True)

    return loss, metric

#############################
# main function boilerplate #
#############################

if __name__=='__main__':
    # TODO: leave these in for now as debugging aids; remove for beta
    # TODO: try cntk_py without _ (feedback from Willi)
    #from cntk import DeviceDescriptor
    #DeviceDescriptor.set_default_device(cntk_device(-1)) # force CPU
    from _cntk_py import set_computation_network_trace_level, set_fixed_random_seed, force_deterministic_algorithms
    set_computation_network_trace_level(1)  # TODO: remove debugging facilities once this all works
    #set_computation_network_trace_level(1000000)  # TODO: remove debugging facilities once this all works
    set_fixed_random_seed(1)  # BUGBUG: has no effect at present  # TODO: remove debugging facilities once this all works
    force_deterministic_algorithms()

    reader = create_reader(data_dir + "/atis.train.ctf", is_training=True)
    model = create_model_function()
    # train
    train(reader, model, max_epochs=8)

    # save and load (as an illustration)
    path = data_dir + "/model.cmf"
    model.save_model(path)
    # BUGBUG: fails with "AttributeError: 'super' object has no attribute 'save_model'"
    model = load_model(path)

    # test
    reader = create_reader(data_dir + "/atis.test.ctf", is_training=False)
    evaluate(reader, model)
