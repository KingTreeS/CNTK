#pragma once

#ifdef __PROFILE__
namespace Chashu
{
/// every 100 iters to print
// convolution
static double convTime = 0.0;
// fully connect
static double tnGatherDistLabelTime = 0.0;
static double tnMatrixMultiplyTime = 0.0;
static double tnDistLabelAddTime = 0.0;
static double tnMatrixScaleTime = 0.0;

static double sdCudaMemcpyAndMPIAllGatherTime = 0.0;
static double sdMPIIallgatherTime = 0.0;
static double sdMPIAllGatherTime = 0.0;
static double sdNCCLAllGatherTime = 0.0;
static double sdNCCLSyncTime = 0.0;
static double sdMPIWaitTime = 0.0;

// Aggregation
static double aggFormListOfSmoothedGradTime = 0.0;
static double aggHoistCriterionToCPUAllreduceTime = 0.0;
static double aggCopyAllValToBeAggregatedToHeaderTime = 0.0;
static double aggAsyncTime = 0.0;
static double aggSwapTime = 0.0;
static double aggCopyGradDataToBufferTime = 0.0;
static double aggInitRecvHeaderAndSendNodes = 0.0;
static double aggNCCLAllReduceTime = 0.0;
static double aggMainNodeWaitAndAggTime = 0.0;
static double aggMPIBcastTime = 0.0;
static double aggNCCLSyncTime = 0.0;
static double aggCopyDataBackToGradTime = 0.0;
static double aggMPIWaitTime = 0.0;
} // namespace Chashu
#endif