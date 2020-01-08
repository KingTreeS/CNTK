//
// Copyright (c) Microsoft. All rights reserved.
// Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#pragma once

#include "ComputationNode.h"

#undef _SCL_SECURE_NO_WARNINGS
#include "Constants.h"
#include "CNTKLibrary.h"
#include "IDistGradAggregator.h"
#include "CUDAPageLockedMemAllocator.h"
#include "NcclComm.h"
#include <future>
#include "GPUDataTransferer.h"
#include "TimerUtility.h"
#include "MatrixQuantizerImpl.h"
#include "ProgressTracing.h"

#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace Microsoft
{
namespace MSR
{
namespace CNTK
{
template <typename T>
class threadsafe_queue
{
private:
    mutable std::mutex mut;
    std::queue<T> data_queue;
    std::condition_variable data_cond;

public:
    threadsafe_queue() {}
    threadsafe_queue(threadsafe_queue const& other) = delete;

    void push(const T& new_value)
    {
        std::lock_guard<std::mutex> lk(mut);
        data_queue.push(new_value);
        data_cond.notify_one();
    }

    void wait_and_pop(T& value)
    {
        std::unique_lock<std::mutex> lk(mut);
        data_cond.wait(lk, [this] { return !data_queue.empty(); });
        value = data_queue.front();
        data_queue.pop();
    }

    bool try_pop(T& value)
    {
        std::lock_guard<std::mutex> lk(mut);
        if (data_queue.empty())
            return false;
        value = data_queue.front();
        data_queue.pop();
        return true;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lk(mut);
        return data_queue.empty();
    }
};

namespace ASYNCNCCL
{
// init in SGD
static std::unique_ptr<NcclComm> m_asyncNccl;

template <typename ElemType>
static std::unordered_map<Matrix<ElemType>*, ElemType*> m_updateGradMap;

template <typename ElemType>
void BackpropWithGradAggNccl(const ComputationNodeBasePtr& node)
{
    if (!node->NeedsGradient())
        return;

    shared_ptr<ComputationNode<ElemType>> gradNode = dynamic_pointer_cast<ComputationNode<ElemType>>(node);
    if (gradNode->IsParameterUpdateRequired() && !gradNode->m_distribute)
    {
        Matrix<ElemType>* currParamsGradient = &(gradNode->Gradient()); // TODO: we can use shared_ptrs now

        // Sometimes, in parallel training, the current node may not get any samples to process
        // In this case, the gradient matrix may not have been sized yet. If so, lets size it.
        if (currParamsGradient->GetNumCols() == 0)
        {
            Matrix<ElemType>* currParamsValues = &(gradNode->Value());
            currParamsGradient->Resize(currParamsValues->GetNumRows(), currParamsValues->GetNumCols());
        }

        // sync main stream
        cudaEvent_t m_asyncEvent;
        cudaEventCreateWithFlags(&m_asyncEvent, cudaEventDisableTiming);
        cudaEventRecord(m_asyncEvent);

        auto rc = cudaEventQuery(m_asyncEvent);
        if (rc == cudaErrorNotReady)
            cudaStreamWaitEvent(cudaStreamDefault, m_asyncEvent, 0) || "cudaEventSynchronize failed";

        cudaEventDestroy(m_asyncEvent);

        if (m_asyncNccl.get() != nullptr)
        {
            size_t elemSize = currParamsGradient->GetNumElements();
            ElemType* reducedGrad;
            cudaMalloc((void**) &reducedGrad, sizeof(ElemType) * elemSize);

			m_updateGradMap<ElemType>[currParamsGradient] = reducedGrad;
            m_asyncNccl->AllReduce(currParamsGradient->Data(), reducedGrad, elemSize);
            // m_asyncNccl->AllReduce(learningParamGrad);
        }
    }
}

template <typename ElemType>
void AsyncUpdateGrad()
{
	for (auto& elem : m_updateGradMap<ElemType>)
	{
        cudaMemcpy(elem.first->Data(), elem.second, elem.first->GetNumElements() * sizeof(ElemType), cudaMemcpyDeviceToDevice);
        cudaFree(elem.second);
	}
    m_updateGradMap<ElemType>.clear();
}
} // namespace ASYNCNCCL

namespace ASYNCMPI
{
static MPIWrapperPtr m_asyncMpi;
static std::unique_ptr<CUDAPageLockedMemAllocator> m_asyncAllocator;
static threadsafe_queue<ComputationNodeBasePtr> m_asyncNodeQueue;

template <typename ElemType>
std::shared_ptr<ElemType> SyncAllocateIntermediateBuffer(int deviceID, size_t numElements)
{
    assert(deviceID >= 0);

    // Use pinned memory for GPU devices for better copy performance
    size_t totalSize = sizeof(ElemType) * numElements;
    return std::shared_ptr<ElemType>((ElemType*) m_asyncAllocator->Malloc(totalSize), [deviceID](ElemType* p) {
        m_asyncAllocator->Free(p);
    });
}

template <typename ElemType>
void AsyncAggreagateGradientsImpl(const std::vector<Matrix<ElemType>*>& gradients)
{
    if (gradients.size() == 0)
        return;

    int deviceId = gradients[0]->GetDeviceId();

    std::vector<std::unique_ptr<GPUDataTransferer>> asyncGpuDataTransferers;
    std::vector<std::shared_ptr<ElemType>> ayncIntermediateCPUBuffers;
    size_t numGradAgg = gradients.size();
    for (size_t i = 0; i < numGradAgg; ++i)
    {
        // Make sure none of the gradient matrixes are sparse - we currently do not support aggregation of sparse gradient matrices
        if (gradients[i]->GetMatrixType() != DENSE)
            RuntimeError("Gradient aggregation for sparse gradient matrices is currently unsupported!");

        // TODO: make these operations thread safety
        asyncGpuDataTransferers.push_back(std::make_unique<GPUDataTransferer>(deviceId, true));
        ayncIntermediateCPUBuffers.push_back(SyncAllocateIntermediateBuffer<ElemType>(deviceId, gradients[i]->GetNumElements()));
    }

    // New aggregation pipeline for non-GDR, perform sync allreduce on the gradient data
    // For CPU, still use async allreduce
    size_t allReduceIndex = 0;
    // non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU
    for (size_t gradIndex = 0; gradIndex < numGradAgg; ++gradIndex)
    {
        Matrix<ElemType>* gpuCopyBuffer = gradients[gradIndex];
        // Async D-to-H copy (next gradient)
        asyncGpuDataTransferers[gradIndex]->CopyGPUToCPUAsync(gpuCopyBuffer->Data(), gpuCopyBuffer->GetNumElements(), ayncIntermediateCPUBuffers[gradIndex].get());

        // Wait for previous copy
        asyncGpuDataTransferers[gradIndex]->WaitForCopyGPUToCPUAsync();

        // Allreduce
        ElemType* reductionBuffer = ayncIntermediateCPUBuffers[gradIndex].get();
        m_asyncMpi->AllReduce(reductionBuffer, gradients[gradIndex]->GetNumElements());

        asyncGpuDataTransferers[gradIndex]->CopyCPUToGPUAsync(ayncIntermediateCPUBuffers[gradIndex].get(), gradients[gradIndex]->GetNumElements(), gradients[gradIndex]->Data());
        ++allReduceIndex;
    }

    // Wait for async CPU-to-GPU copy (non-GDR)
    for (size_t i = 0; i < allReduceIndex; ++i)
        asyncGpuDataTransferers[i]->WaitForCopyCPUToGPUAsync();

    asyncGpuDataTransferers.clear();
    ayncIntermediateCPUBuffers.clear();
}

template <typename ElemType>
void AsyncAggreagateGradients(const ComputationNodeBasePtr& node)
{
    if (m_asyncMpi->NumNodesInUse() == 1) // No need to aggregate anything.
        return;

    if (!node->NeedsGradient())
        return;

    m_asyncNodeQueue.push(node);
}

template <typename ElemType>
void BackpropAsyncMpiThread(const atomic_bool& asyncMpiFlag)
{
    if (m_asyncMpi->NumNodesInUse() == 1) // No need to aggregate anything.
        return;

    while (true)
    {
        if (asyncMpiFlag && m_asyncNodeQueue.empty())
            break;

        while (!m_asyncNodeQueue.empty())
        {
            ComputationNodeBasePtr node;
            m_asyncNodeQueue.try_pop(node);

            shared_ptr<ComputationNode<ElemType>> gradNode = std::dynamic_pointer_cast<ComputationNode<ElemType>>(node);

            if (gradNode->IsParameterUpdateRequired() && !gradNode->m_distribute)
            {
                Matrix<ElemType>* currParamsGradient = &(gradNode->Gradient()); // TODO: we can use shared_ptrs now

                cudaSetDevice(currParamsGradient->GetDeviceId());
                // Sometimes, in parallel training, the current node may not get any samples to process
                // In this case, the gradient matrix may not have been sized yet. If so, lets size it.
                if (currParamsGradient->GetNumCols() == 0)
                {
                    Matrix<ElemType>* currParamsValues = &(gradNode->Value());
                    currParamsGradient->Resize(currParamsValues->GetNumRows(), currParamsValues->GetNumCols());
                }

                std::vector<Matrix<ElemType>*> learningParamGrad;
                learningParamGrad.push_back(currParamsGradient);

                // sync main stream

                cudaEvent_t m_asyncEvent;
                cudaEventCreateWithFlags(&m_asyncEvent, cudaEventDisableTiming);
                cudaEventRecord(m_asyncEvent);

                auto rc = cudaEventQuery(m_asyncEvent);
                if (rc == cudaErrorNotReady)
                    cudaStreamWaitEvent(cudaStreamDefault, m_asyncEvent, 0) || "cudaEventSynchronize failed";

                cudaEventDestroy(m_asyncEvent);

                AsyncAggreagateGradientsImpl<ElemType>(learningParamGrad);
            }
        }
    }
}
} // namespace ASYNCMPI

static size_t profileCnt = 0;

template <class ElemType>
class SimpleDistGradAggregator : public IDistGradAggregator<ElemType>
{
    UsingIDistGradAggregatorMembers;

public:
    SimpleDistGradAggregator(const MPIWrapperPtr& mpi, bool useAsyncAggregation, int deviceId, int syncStatsTrace, size_t packThresholdSizeInBytes = DEFAULT_PACK_THRESHOLD_SIZE_IN_BYTES)
        : IDistGradAggregator<ElemType>(mpi), m_useAsyncAggregation(useAsyncAggregation), m_initialized(false), m_bufferedGradHeader(nullptr), m_syncStatsTrace(syncStatsTrace), m_iterationCount(0), m_packThresholdSizeInBytes(packThresholdSizeInBytes)
    {
    }

    ~SimpleDistGradAggregator()
    {
        for (size_t i = 0; i < m_recvHeaders.size(); ++i)
            DistGradHeader::Destroy(m_recvHeaders[i]);

        if (m_bufferedGradHeader != nullptr)
            DistGradHeader::Destroy(m_bufferedGradHeader);
    }

    // Aggregate the gradient matrices across all nodes
    bool AggregateGradients(const std::vector<Matrix<ElemType>*>& gradients, DistGradHeader* headerCPU, bool resetState) override
    {
        //detail profile
        std::chrono::time_point<std::chrono::system_clock> sdStartTime;
        std::chrono::time_point<std::chrono::system_clock> sdEndTime;

        if (m_mpi->NumNodesInUse() == 1) // No need to aggregate anything.
            return (headerCPU->numSamples != 0);

        // Initialize NCCL
        if (m_nccl == nullptr)
            m_nccl.reset(new NcclComm(::CNTK::DeviceDescriptor::UseDefaultDevice().Id(), m_mpi));

        ResetState(gradients, headerCPU->numEvalNode, resetState);
        bool showSyncPerfStats = (m_syncStatsTrace > 0) && ((m_iterationCount % m_syncStatsTrace) == 0);
        m_iterationCount++;

        if (m_useAsyncAggregation)
        {
            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                if (profileCnt % 100 == 0)
                    LOGPRINTF(stderr, "Aggregation: Use Async Aggregation\n");

                sdStartTime = std::chrono::system_clock::now();
            }

            // If we are performing async gradient aggregation, let's wait for the pending gradient aggregation to finish
            // then swap the contents of the buffered gradients and the new gradient matrices and fire an async aggreagation
            // of the new gradient matrices
            if (m_pendingAsyncAggregation.valid())
            {
                Timer aggregationTimer;
                if (showSyncPerfStats)
                    aggregationTimer.Start();

                m_pendingAsyncAggregation.get();

                if (showSyncPerfStats)
                {
                    aggregationTimer.Stop();
                    double gradientAggregationTime = aggregationTimer.ElapsedSeconds();
                    fprintf(stderr, "Async gradient aggregation wait time: %.6g\n", gradientAggregationTime);
                }
            }

            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                sdEndTime = std::chrono::system_clock::now();
                Chashu::aggAsyncTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

                sdStartTime = std::chrono::system_clock::now();
            }

            std::vector<Matrix<ElemType>*> newGradients;
            size_t numGradMatrices = gradients.size();
            for (size_t i = 0; i < numGradMatrices; i++)
            {
                Matrix<ElemType>* bufferedGradientMatrix = m_bufferedGradients[gradients[i]].get();
                if ((bufferedGradientMatrix == nullptr) ||
                    (bufferedGradientMatrix->GetNumCols() != gradients[i]->GetNumCols()) ||
                    (bufferedGradientMatrix->GetNumRows() != gradients[i]->GetNumRows()) ||
                    (bufferedGradientMatrix->GetDeviceId() != gradients[i]->GetDeviceId()))
                {
                    LogicError("No buffered gradient matrix found corresponding to a gradient matrix to be aggregated!");
                }

                // Swap the gradient matrix contents with the buffered matrices
                std::swap(*(gradients[i]), *bufferedGradientMatrix);

                newGradients.push_back(bufferedGradientMatrix);
            }

            // Swap the grad header contents with the buffered grad header
            swap(*headerCPU, *m_bufferedGradHeader);

            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                sdEndTime = std::chrono::system_clock::now();
                Chashu::aggSwapTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
            }

            // Initiate aggregation only if any samples were processed in previous iteration
            if (resetState || (headerCPU->numSamples != 0))
            {
                int deviceId = gradients[0]->GetDeviceId();
                DistGradHeader* newGradHeader = m_bufferedGradHeader;

                // Since we will be aggregating the gradients assynchronously, let us
                // ensure that the gradient matrices have been computed before starting to aggregate
                // them asynchronously on another thread. This essentially means that when we are using
                // a GPU device, we will synchronize on the main GPU compute stream before starting
                // the gradient aggregation asynchronously on a separate stream
                MatrixComputeStreamEvent* mainStreamSyncEvent = MatrixComputeStreamEvent::Create(deviceId);

                m_pendingAsyncAggregation = std::async(std::launch::async, [=] {
                    // We are starting on a new thread. Make sure the new thread is
                    // setup to use the right device
                    Matrix<ElemType>::SetDevice(deviceId);

                    // Synchronize the Quantization compute stream with the completion of
                    // compute of the gradient matrices on the main compute stream
                    mainStreamSyncEvent->SynchronizeDataTransferFetchStreamWithEvent<ElemType>();
                    delete mainStreamSyncEvent;

                    AggregateGradientsImpl(newGradients, newGradHeader, showSyncPerfStats);
                });

                return true;
            }

            return false;
        }
        else
        {
            AggregateGradientsImpl(gradients, headerCPU, showSyncPerfStats);
            return (headerCPU->numSamples != 0);
        }
    }

    bool AsyncAggregateGradHeader(DistGradHeader* headerCPU) override
    {
        if (m_mpi->NumNodesInUse() == 1) // No need to aggregate anything.
            return (headerCPU->numSamples != 0);

        // Initialize NCCL
        if (m_nccl == nullptr)
            m_nccl.reset(new NcclComm(::CNTK::DeviceDescriptor::UseDefaultDevice().Id(), m_mpi));

        if (m_mpi->IsMainNode())
        {
            for (size_t i = 0; i < NumProc() - 1; ++i)
                m_recvHeaders.push_back(DistGradHeader::Create(headerCPU->numEvalNode));
        }

        AsyncAggregateGradHeaderImpl(headerCPU);
        return (headerCPU->numSamples != 0);
    }

private:
    std::shared_ptr<ElemType> AllocateIntermediateBuffer(int deviceID, size_t numElements)
    {
        assert(deviceID >= 0);

        // Use pinned memory for GPU devices for better copy performance
        size_t totalSize = sizeof(ElemType) * numElements;
        return std::shared_ptr<ElemType>((ElemType*) m_allocator->Malloc(totalSize), [this, deviceID](ElemType* p) {
            m_allocator->Free(p);
        });
    }

    bool ShouldCopyDataToCPU(int deviceId)
    {
        // Do not copy if data is on CPU
        if (deviceId == CPUDEVICE)
            return false;

        // Do not copy if NCCL is supported or GPUDirect RDMA is used
        if (m_nccl->IsSupported() || m_mpi->UseGpuGdr() == true)
            return false;

        return true;
    }

    void ResetState(const std::vector<Matrix<ElemType>*>& gradients, int numEvalNodes, bool resetState)
    {
        // When called the first time let's setup the intermediateCPU buffers for gradient aggregation if needed
        if (!m_initialized)
        {
            m_initialized = true;
            int deviceId = gradients[0]->GetDeviceId();

            // Initial preparation for data copy from GPU to CPU
            if (ShouldCopyDataToCPU(deviceId) && m_allocator.get() == nullptr)
            {
                m_allocator.reset(new CUDAPageLockedMemAllocator(deviceId));
            }

            size_t packedGradientsSizeInElements = 0;
            for (size_t i = 0; i < gradients.size(); i++)
            {
                if (!m_useAsyncAggregation && sizeof(ElemType) * gradients[i]->GetNumElements() <= m_packThresholdSizeInBytes)
                {
                    packedGradientsSizeInElements += gradients[i]->GetNumElements();
                    m_packedGradientsIndex.push_back(i);
                }
                else
                {
                    m_gradientIndexToAggregate.push_back(i);
                }

                // Make sure none of the gradient matrixes are sparse - we currently do not support aggregation of sparse gradient matrices
                if (gradients[i]->GetMatrixType() != DENSE)
                    RuntimeError("Gradient aggregation for sparse gradient matrices is currently unsupported!");

                if (m_useAsyncAggregation)
                    m_bufferedGradients[gradients[i]].reset(new Matrix<ElemType>(gradients[i]->GetNumRows(), gradients[i]->GetNumCols(), deviceId));
            }

            // Packing matrices into continous buffer if not doing async aggregation
            m_aggregationBuffer.reset();
            if (packedGradientsSizeInElements > 0)
            {
                m_aggregationBuffer.reset(new (std::nothrow) Matrix<ElemType>(1, packedGradientsSizeInElements, deviceId));
            }
            // If no extra continous buffer allocated or using async aggregation
            if (m_aggregationBuffer == nullptr)
            {
                m_gradientIndexToAggregate.clear();
                m_packedGradientsIndex.clear();
                packedGradientsSizeInElements = 0;
                // Reuse "@param m_gradientIndexToAggregate" for following code, if no continous buffer allocated
                for (size_t i = 0; i < gradients.size(); i++)
                {
                    m_gradientIndexToAggregate.push_back(i);
                }
            }
            else
            {
                // First element is reserved for continous buffer
                m_gradientIndexToAggregate.insert(m_gradientIndexToAggregate.begin(), 1, (size_t) -1);
            }

            if (ShouldCopyDataToCPU(deviceId))
            {
                for (size_t i : m_gradientIndexToAggregate)
                {
                    m_gpuDataTransferers.push_back(std::make_unique<GPUDataTransferer>(deviceId, m_useAsyncAggregation));
                    m_intermediateCPUBuffers.push_back(AllocateIntermediateBuffer(deviceId,
                                                                                  (i == -1) ? packedGradientsSizeInElements : gradients[i]->GetNumElements()));
                }
            }

            if (m_useAsyncAggregation)
            {
                m_bufferedGradHeader = DistGradHeader::Create(numEvalNodes);
                m_bufferedGradHeader->Clear();
            }

            if (m_mpi->IsMainNode())
            {
                for (size_t i = 0; i < NumProc() - 1; ++i)
                    m_recvHeaders.push_back(DistGradHeader::Create(numEvalNodes));
            }
        }
        else if (resetState)
        {
            // Make sure there is no pending async aggregation
            if (m_useAsyncAggregation && m_pendingAsyncAggregation.valid())
                LogicError("Unexpected pending async gradient aggregation found when resetting aggregator state!");

            // Zero out the buffered gradients if resetting state
            if (m_useAsyncAggregation)
            {
                for (size_t i = 0; i < gradients.size(); i++)
                    m_bufferedGradients[gradients[i]]->SetValue(0);

                m_bufferedGradHeader->Clear();
            }
        }
    }

    void AsyncResetData(const std::vector<Matrix<ElemType>*>& gradients)
    {
        int deviceId = gradients[0]->GetDeviceId();
        if (!ShouldCopyDataToCPU(deviceId))
            return;

        // Initial preparation for data copy from GPU to CPU
        if (m_asyncAllocator.get() == nullptr)
            m_asyncAllocator.reset(new CUDAPageLockedMemAllocator(deviceId));

        m_asyncGpuDataTransferers.clear();
        m_ayncIntermediateCPUBuffers.clear();
        for (size_t i = 0; i < gradients.size(); ++i)
        {
            // Make sure none of the gradient matrixes are sparse - we currently do not support aggregation of sparse gradient matrices
            if (gradients[i]->GetMatrixType() != DENSE)
                RuntimeError("Gradient aggregation for sparse gradient matrices is currently unsupported!");

            // TODO: make these operations thread safety
            m_asyncGpuDataTransferers.push_back(std::make_unique<GPUDataTransferer>(deviceId, true));
            m_ayncIntermediateCPUBuffers.push_back(SyncAllocateIntermediateBuffer(deviceId, gradients[i]->GetNumElements()));
        }
    }

    void AggregateGradientsImpl(const std::vector<Matrix<ElemType>*>& gradients, DistGradHeader* headerCPU, bool showSyncPerfStats)
    {
        Timer aggregationTimer;
        int deviceId = gradients[0]->GetDeviceId();
        if (showSyncPerfStats)
        {
            std::unique_ptr<MatrixComputeStreamEvent> mainStreamSyncEvent(MatrixComputeStreamEvent::Create(deviceId));
            mainStreamSyncEvent->SynchronizeEvent();
            aggregationTimer.Start();
        }

        size_t numGradMatrices = gradients.size();

        if (headerCPU->numSamples == 0)
        {
            assert(headerCPU->criterion == 0.0);
            assert(headerCPU->numSamplesWithLabel == 0);
            for (int i = 0; i < headerCPU->numEvalNode; ++i)
                assert(headerCPU->evalErrors[i].first == 0 && headerCPU->evalErrors[i].second == 0);

            // If the current node did not process any samples, the gradients should be zero'd
            for (size_t i = 0; i < numGradMatrices; ++i)
                gradients[i]->SetValue(0);

            if (m_useAsyncAggregation)
            {
                std::unique_ptr<MatrixComputeStreamEvent> mainStreamSyncEvent(MatrixComputeStreamEvent::Create(deviceId));
                mainStreamSyncEvent->SynchronizeDataTransferFetchStreamWithEvent<ElemType>();
            }
        }

        // detail profile
        std::chrono::time_point<std::chrono::system_clock> sdStartTime;
        std::chrono::time_point<std::chrono::system_clock> sdEndTime;

        sdStartTime = std::chrono::system_clock::now();

        // Copy all gradient data into a single contiguous buffer, if additional continous buffer allocated
        size_t offset = 0;
        for (size_t i : m_packedGradientsIndex)
        {
            m_aggregationBuffer->ColumnSlice(offset, gradients[i]->GetNumElements()).AssignValuesOf(gradients[i]->Reshaped(1, gradients[i]->GetNumElements()));
            offset += gradients[i]->GetNumElements();
        }

        if (strcmp(Chashu::detailProfile, "TRUE") == 0)
        {
            sdEndTime = std::chrono::system_clock::now();
            Chashu::aggCopyGradDataToBufferTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

            sdStartTime = std::chrono::system_clock::now();
        }

        // Initiate receive of the header on the main node
        std::vector<MPI_Request> recvHeaderRequests(NumProc() - 1);
        if (m_mpi->IsMainNode())
        {
            for (size_t j = 0; j < NumProc() - 1; ++j)
            {
                int source = (j >= MyRank()) ? (j + 1) : j;
                // We use a tag of 'numGradMatrices' for the pre-aggregation header
                m_mpi->Irecv(m_recvHeaders[j], m_recvHeaders[j]->Size(), MPI_CHAR, source, numGradMatrices, &(recvHeaderRequests[j])) || MpiFail("MPI_Irecv");
            }
        }

        // Send the headers from all nodes but the main node
        MPI_Request sendHeaderRequest;
        if (!m_mpi->IsMainNode())
            m_mpi->Isend(headerCPU, headerCPU->Size(), MPI_CHAR, m_mpi->MainNodeRank(), numGradMatrices, &sendHeaderRequest) || MpiFail("MPI_Isend");

        if (strcmp(Chashu::detailProfile, "TRUE") == 0)
        {
            sdEndTime = std::chrono::system_clock::now();
            Chashu::aggInitRecvHeaderAndSendNodes += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
        }

        // New aggregation pipeline for non-GDR, perform sync allreduce on the gradient data
        // For CPU, still use async allreduce
        std::vector<MPI_Request> allReduceRequests;
        size_t gpuToCpuIndex = 0;
        size_t cpuToGpuIndex = 0;
        size_t allReduceIndex = 0;
        size_t numGradientIndex = m_gradientIndexToAggregate.size();
        if (numGradientIndex > 0)
        {
            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                if (profileCnt % 100 == 0)
                {
                    LOGPRINTF(stderr, "AggregateGradientsImpl: m_mpi->UseGpuGdr() = %d\n", m_mpi->UseGpuGdr());
                    LOGPRINTF(stderr, "AggregateGradientsImpl: deviceId = %d\n", deviceId);
                    LOGPRINTF(stderr, "AggregateGradientsImpl: m_nccl->IsSupported() = %d\n", m_nccl->IsSupported());
                }
            }

            // non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU
            if ((m_mpi->UseGpuGdr() == 0) && (deviceId != CPUDEVICE) && !m_nccl->IsSupported())
            {
                if (strcmp(Chashu::detailProfile, "TRUE") == 0)
                {
                    if (profileCnt++ % 100 == 0)
                        LOGPRINTF(stderr, "AggregateGradientsImpl Branch1[non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU] : m_mpi->UseGpuGdr() == false && deviceId != CPUDEVICE && m_nccl->IsSupported() == false \n");
                }

                Matrix<ElemType>* gpuCopyBuffer = m_aggregationBuffer.get();

                ElemType* reductionBuffer;
                // currentGradientIndex will load the index from m_gradientIndexToAggregate
                size_t currentGradientIndex = m_gradientIndexToAggregate[0];
                size_t nextGradientIndex = 0; // 0 is for initialization only
                // Get the first Gradient, and do async D-to-H copy
                if (currentGradientIndex != -1)
                {
                    gpuCopyBuffer = gradients[currentGradientIndex];
                }
                else
                {
                    // currentGradientIndex == -1, first element is for packed gradients, which should not be with AsyncAggregation
                    assert(m_useAsyncAggregation == false);
                }
// First sync_g_to_c_copy
// TODO: we need a CopyGPUToCPUSync
#ifndef CPUONLY
                cudaMemcpy(m_intermediateCPUBuffers[gpuToCpuIndex].get(), gpuCopyBuffer->Data(), gpuCopyBuffer->GetNumElements() * sizeof(ElemType), cudaMemcpyDeviceToHost);
#endif
                gpuToCpuIndex++;

                for (size_t i = 1; i <= numGradientIndex; i++)
                {
                    // Get next gradient
                    if (i < numGradientIndex)
                    {
                        nextGradientIndex = m_gradientIndexToAggregate[i];
                        if (nextGradientIndex != -1)
                        {
                            gpuCopyBuffer = gradients[nextGradientIndex];
                        }
                        else
                        {
                            // currentGradientIndex == -1, first element is for packed gradients, which should not be with AsyncAggregation
                            assert(m_useAsyncAggregation == false);
                        }
                        // Async D-to-H copy (next gradient)
                        m_gpuDataTransferers[gpuToCpuIndex]->CopyGPUToCPUAsync(gpuCopyBuffer->Data(), gpuCopyBuffer->GetNumElements(), m_intermediateCPUBuffers[gpuToCpuIndex].get());
                    }
                    // Wait for previous copy
                    m_gpuDataTransferers[allReduceIndex]->WaitForCopyGPUToCPUAsync();

                    // Allreduce
                    reductionBuffer = m_intermediateCPUBuffers[allReduceIndex].get();
                    m_mpi->AllReduce(reductionBuffer, (currentGradientIndex == -1) ? m_aggregationBuffer->GetNumElements() : gradients[currentGradientIndex]->GetNumElements());

                    // Create async H-to-G copy
                    cpuToGpuIndex = allReduceIndex;
                    m_gpuDataTransferers[cpuToGpuIndex]->CopyCPUToGPUAsync(m_intermediateCPUBuffers[cpuToGpuIndex].get(),
                                                                           (currentGradientIndex == -1) ? m_aggregationBuffer->GetNumElements() : gradients[currentGradientIndex]->GetNumElements(),
                                                                           (currentGradientIndex == -1) ? m_aggregationBuffer->Data() : gradients[currentGradientIndex]->Data());
                    allReduceIndex = gpuToCpuIndex;
                    gpuToCpuIndex++;
                    currentGradientIndex = nextGradientIndex;
                }
            }
            // non-NCCL, using CPU, using GDR
            else if (!m_nccl->IsSupported())
            {
                if (strcmp(Chashu::detailProfile, "TRUE") == 0)
                {
                    if (profileCnt++ % 100 == 0)
                        LOGPRINTF(stderr, "AggregateGradientsImpl Branch2[non-NCCL, using CPU, using GDR] : m_nccl->IsSupported() == false \n");
                }

                ElemType* reductionBuffer;
                for (size_t i : m_gradientIndexToAggregate)
                {
                    allReduceRequests.push_back(MPI_Request());
                    reductionBuffer = (i == -1) ? m_aggregationBuffer->Data() : gradients[i]->Data();
                    // CPU
                    if (m_mpi->UseGpuGdr() == 0)
                    {
                        m_mpi->Iallreduce(MPI_IN_PLACE, reductionBuffer, (i == -1) ? m_aggregationBuffer->GetNumElements() : gradients[i]->GetNumElements(),
                                          MPIWrapper::GetDataType(reductionBuffer), MPI_SUM, &allReduceRequests.back()) ||
                            MpiFail("MPI_Iallreduce");
                        allReduceIndex++;
                    }
                    // GDR && GPU
                    else if (deviceId != CPUDEVICE)
                    {
                        m_mpi->AllReduce(reductionBuffer, (i == -1) ? m_aggregationBuffer->GetNumElements() : gradients[i]->GetNumElements());
                    }
                }
            }
            else if (m_nccl->IsSupported())
            {
                if (strcmp(Chashu::detailProfile, "TRUE") == 0)
                {
                    if (profileCnt++ % 100 == 0)
                        LOGPRINTF(stderr, "AggregateGradientsImpl Branch3 : m_nccl->IsSupported() == true \n");

                    sdStartTime = std::chrono::system_clock::now();
                }

                std::vector<Matrix<ElemType>*> ncclReduceGradients;
                for (size_t i : m_gradientIndexToAggregate)
                {
                    ncclReduceGradients.push_back((i == -1) ? m_aggregationBuffer.get() : gradients[i]);
                }
                m_nccl->AllReduce(ncclReduceGradients);

                if (strcmp(Chashu::detailProfile, "TRUE") == 0)
                {
                    sdEndTime = std::chrono::system_clock::now();
                    Chashu::aggNCCLAllReduceTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
                }
            }
        }

        if (strcmp(Chashu::detailProfile, "TRUE") == 0)
        {
            sdStartTime = std::chrono::system_clock::now();
        }

        // On the main node wait for the headers to arrive and aggregate
        if (m_mpi->IsMainNode())
        {
            size_t numNodesHeadersReceivedFrom = 0;
            while (numNodesHeadersReceivedFrom < (NumProc() - 1))
            {
                int idx = MPI_UNDEFINED;
                m_mpi->Waitany(recvHeaderRequests.size(), recvHeaderRequests.data(), &idx, MPI_STATUS_IGNORE) || MpiFail("MPI_Waitany");
                if (idx == MPI_UNDEFINED)
                {
                    break;
                }

                numNodesHeadersReceivedFrom++;

                headerCPU->Aggregate(m_recvHeaders[idx], true);
            }

            assert(numNodesHeadersReceivedFrom == (NumProc() - 1));
        }

        if (strcmp(Chashu::detailProfile, "TRUE") == 0)
        {
            sdEndTime = std::chrono::system_clock::now();
            Chashu::aggMainNodeWaitAndAggTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

            sdStartTime = std::chrono::system_clock::now();
        }

        // Broadcast the aggregated header to all nodes
        m_mpi->Bcast(headerCPU, headerCPU->Size(), MPI_CHAR, m_mpi->MainNodeRank());

        if (strcmp(Chashu::detailProfile, "TRUE") == 0)
        {
            sdEndTime = std::chrono::system_clock::now();
            Chashu::aggMPIBcastTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

            sdStartTime = std::chrono::system_clock::now();
        }

        if (m_nccl->IsSupported())
        {
            m_nccl->Sync();

            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                sdEndTime = std::chrono::system_clock::now();
                Chashu::aggNCCLSyncTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
            }
        }
        // Non-GDR && GPU
        else if ((m_mpi->UseGpuGdr() == 0) && (deviceId != CPUDEVICE))
        {
            // Wait for async CPU-to-GPU copy (non-GDR)
            for (size_t i = 0; i < allReduceIndex; i++)
                m_gpuDataTransferers[i]->WaitForCopyCPUToGPUAsync();
        }
        // CPU
        else if (m_mpi->UseGpuGdr() == 0)
        {
            // Wait for the Iallreduce operations to finish
            for (size_t i = 0; i < allReduceIndex; i++)
            {
                m_mpi->Wait(&allReduceRequests[i], MPI_STATUSES_IGNORE) || MpiFail("MPI_Wait");
            }
        }

        if (strcmp(Chashu::detailProfile, "TRUE") == 0)
        {
            sdStartTime = std::chrono::system_clock::now();
        }

        // Copy data back to the packed gradients from the continous buffer
        offset = 0;
        for (size_t i : m_packedGradientsIndex)
        {
            gradients[i]->AssignValuesOf(m_aggregationBuffer->ColumnSlice(offset, gradients[i]->GetNumElements()).Reshaped(gradients[i]->GetNumRows(), gradients[i]->GetNumCols()));
            offset += gradients[i]->GetNumElements();
        }

        if (strcmp(Chashu::detailProfile, "TRUE") == 0)
        {
            sdEndTime = std::chrono::system_clock::now();
            Chashu::aggCopyDataBackToGradTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

            sdStartTime = std::chrono::system_clock::now();
        }

        // Wait for completion of the async send requests
        if (!m_mpi->IsMainNode())
            m_mpi->Wait(&sendHeaderRequest, MPI_STATUSES_IGNORE) || MpiFail("MPI_Wait");

        if (strcmp(Chashu::detailProfile, "TRUE") == 0)
        {
            sdEndTime = std::chrono::system_clock::now();
            Chashu::aggMPIWaitTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
        }

        if (showSyncPerfStats)
        {
            aggregationTimer.Stop();
            double gradientAggregationTime = aggregationTimer.ElapsedSeconds();
            fprintf(stderr, "Actual gradient aggregation time: %.6g\n", gradientAggregationTime);
        }
    }

    void AsyncAggregateGradHeaderImpl(DistGradHeader* headerCPU)
    {
        int numHeaderSample = headerCPU->numSamples;
        if (numHeaderSample == 0)
        {
            assert(headerCPU->criterion == 0.0);
            assert(headerCPU->numSamplesWithLabel == 0);
            for (int i = 0; i < headerCPU->numEvalNode; ++i)
                assert(headerCPU->evalErrors[i].first == 0 && headerCPU->evalErrors[i].second == 0);
        }

        // Initiate receive of the header on the main node
        std::vector<MPI_Request> recvHeaderRequests(NumProc() - 1);
        if (m_mpi->IsMainNode())
        {
            for (size_t j = 0; j < NumProc() - 1; ++j)
            {
                int source = (j >= MyRank()) ? (j + 1) : j;
                // We use a tag of 'numGradMatrices' for the pre-aggregation header
                m_mpi->Irecv(m_recvHeaders[j], m_recvHeaders[j]->Size(), MPI_CHAR, source, numHeaderSample, &(recvHeaderRequests[j])) || MpiFail("MPI_Irecv");
            }
        }

        // Send the headers from all nodes but the main node
        MPI_Request sendHeaderRequest;
        if (!m_mpi->IsMainNode())
            m_mpi->Isend(headerCPU, headerCPU->Size(), MPI_CHAR, m_mpi->MainNodeRank(), numHeaderSample, &sendHeaderRequest) || MpiFail("MPI_Isend");

        // On the main node wait for the headers to arrive and aggregate
        if (m_mpi->IsMainNode())
        {
            size_t numNodesHeadersReceivedFrom = 0;
            while (numNodesHeadersReceivedFrom < (NumProc() - 1))
            {
                int idx = MPI_UNDEFINED;
                m_mpi->Waitany(recvHeaderRequests.size(), recvHeaderRequests.data(), &idx, MPI_STATUS_IGNORE) || MpiFail("MPI_Waitany");
                if (idx == MPI_UNDEFINED)
                    break;

                ++numNodesHeadersReceivedFrom;

                headerCPU->Aggregate(m_recvHeaders[idx], true);
            }

            assert(numNodesHeadersReceivedFrom == (NumProc() - 1));
        }

        // Broadcast the aggregated header to all nodes
        m_mpi->Bcast(headerCPU, headerCPU->Size(), MPI_CHAR, m_mpi->MainNodeRank());

        // Wait for completion of the async send requests
        if (!m_mpi->IsMainNode())
            m_mpi->Wait(&sendHeaderRequest, MPI_STATUSES_IGNORE) || MpiFail("MPI_Wait");
    }

    bool DistributedCheck(size_t minibatchSize, size_t processNum)
    {
        size_t* gatherBuffer = new size_t[processNum];
        m_mpi->AllGather(&minibatchSize, (size_t) 1, gatherBuffer, (size_t) 1);
        for (size_t i(1); i < processNum; ++i)
        {
            if (gatherBuffer[i] != gatherBuffer[0])
            {
                delete[] gatherBuffer;
                return false;
            }
        }
        delete[] gatherBuffer;
        return true;
    }

    void DistributedInit(DEVICEID_TYPE deviceId, size_t bufferSize)
    {
        if (m_mpi->NumNodesInUse() == 1)
            return;
        if (m_nccl == nullptr)
            m_nccl.reset(new NcclComm(::CNTK::DeviceDescriptor::UseDefaultDevice().Id(), m_mpi));
        if (ShouldCopyDataToCPU(deviceId))
        {
            if (m_allocator.get() == nullptr)
                m_allocator.reset(new CUDAPageLockedMemAllocator(deviceId));
            m_intermediateDistributedCPUBuffer1 = AllocateIntermediateBuffer(deviceId, bufferSize);
            m_intermediateDistributedCPUBuffer2 = AllocateIntermediateBuffer(deviceId, bufferSize);

            if (m_asyncAllocator.get() == nullptr)
                m_asyncAllocator.reset(new CUDAPageLockedMemAllocator(deviceId));
        }
    }

    void DistributedAllGather(const Matrix<ElemType>& distributedMatrix, Matrix<ElemType>& gatheredMatrix, size_t count)
    {
        // detail profile
        std::chrono::time_point<std::chrono::system_clock> sdStartTime;
        std::chrono::time_point<std::chrono::system_clock> sdEndTime;

        int deviceId = distributedMatrix.GetDeviceId();
        MPI_Request allGatherRequest;
        ElemType* distributedMatrixBuffer = distributedMatrix.Data();
        ElemType* gatheredMatrixBuffer = gatheredMatrix.Data();

        if ((m_mpi->UseGpuGdr() == 0) && (deviceId != CPUDEVICE) && !m_nccl->IsSupported()) // non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU
        {
            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                if (profileCnt % 100 == 0)
                    LOGPRINTF(stderr, "DistributedAllGather Branch1[non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU] : m_mpi->UseGpuGdr() == false && deviceId != CPUDEVICE && m_nccl->IsSupported() == false \n");
                sdStartTime = std::chrono::system_clock::now();
            }

            cudaMemcpy(m_intermediateDistributedCPUBuffer1.get(), distributedMatrixBuffer, count * sizeof(ElemType), cudaMemcpyDeviceToHost);
            m_mpi->AllGather(m_intermediateDistributedCPUBuffer1.get(), count, m_intermediateDistributedCPUBuffer2.get(), count);
            cudaMemcpy(gatheredMatrixBuffer, m_intermediateDistributedCPUBuffer2.get(), gatheredMatrix.GetNumElements() * sizeof(ElemType), cudaMemcpyHostToDevice);

            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                sdEndTime = std::chrono::system_clock::now();
                Chashu::sdCudaMemcpyAndMPIAllGatherTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
            }
        }
        else if (!m_nccl->IsSupported()) // non-NCCL, using CPU, using GDR
        {
            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                if (profileCnt % 100 == 0)
                    LOGPRINTF(stderr, "DistributedAllGather Branch2[non-NCCL, using CPU, using GDR] : m_nccl->IsSupported() == false \n");
                sdStartTime = std::chrono::system_clock::now();
            }
            if (m_mpi->UseGpuGdr() == 0) // CPU
            {
                if (strcmp(Chashu::detailProfile, "TRUE") == 0)
                {
                    if (profileCnt % 100 == 0)
                        LOGPRINTF(stderr, "DistributedAllGather Branch2.1[non-NCCL, using CPU]\n");
                }

                m_mpi->Iallgather(distributedMatrixBuffer, gatheredMatrixBuffer, count,
                                  MPIWrapper::GetDataType(distributedMatrixBuffer), &allGatherRequest) ||
                    MpiFail("MPI_Iallgather");

                if (strcmp(Chashu::detailProfile, "TRUE") == 0)
                {
                    sdEndTime = std::chrono::system_clock::now();
                    Chashu::sdMPIIallgatherTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
                }
            }
            else if (deviceId != CPUDEVICE) // GDR && GPU
            {
                if (strcmp(Chashu::detailProfile, "TRUE") == 0)
                {
                    if (profileCnt % 100 == 0)
                        LOGPRINTF(stderr, "DistributedAllGather Branch2.2[non-NCCL, using GPU&GDR]\n");
                }

                m_mpi->AllGather(distributedMatrixBuffer, count, gatheredMatrixBuffer, count);

                if (strcmp(Chashu::detailProfile, "TRUE") == 0)
                {
                    sdEndTime = std::chrono::system_clock::now();
                    Chashu::sdMPIAllGatherTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
                }
            }
            else
                LogicError("LogicError in SimpleDistGradAggregator::DistributedAllGather");
        }
        else if (m_nccl->IsSupported()) // NCCL
        {
            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                if (profileCnt % 100 == 0)
                    LOGPRINTF(stderr, "DistributedAllGather Branch3 : m_nccl->IsSupported() == true \n");
                sdStartTime = std::chrono::system_clock::now();
            }

            m_nccl->AllGather(distributedMatrixBuffer, gatheredMatrixBuffer, count);

            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                sdEndTime = std::chrono::system_clock::now();
                Chashu::sdNCCLAllGatherTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
            }
        }
        else
            LogicError("LogicError in SimpleDistGradAggregator::DistributedAllGather");

        if (strcmp(Chashu::detailProfile, "TRUE") == 0)
        {
            sdStartTime = std::chrono::system_clock::now();
        }

        if (m_nccl->IsSupported()) // Non-GDR && GPU
        {
            m_nccl->Sync();

            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                sdEndTime = std::chrono::system_clock::now();
                Chashu::sdNCCLSyncTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
            }
        }
        else if ((m_mpi->UseGpuGdr() == 0) && (deviceId != CPUDEVICE))
        {
        }
        else if (m_mpi->UseGpuGdr() == 0) // CPU
        {
            m_mpi->Wait(&allGatherRequest, MPI_STATUSES_IGNORE) || MpiFail("MPI_Wait"); // Wait for the Iallreduce operations to finish

            if (strcmp(Chashu::detailProfile, "TRUE") == 0)
            {
                sdEndTime = std::chrono::system_clock::now();
                Chashu::sdMPIWaitTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();
            }
        }
        else
            LogicError("LogicError in SimpleDistGradAggregator::DistributedAllGather");
    }

    void DistributedAllReduce(const Matrix<ElemType>& distributedMatrix, MPI_Op op)
    {
        int deviceId = distributedMatrix.GetDeviceId();
        MPI_Request allReduceRequest;
        ElemType* distributedMatrixBuffer = distributedMatrix.Data();
        size_t count = distributedMatrix.GetNumElements();

        if ((m_mpi->UseGpuGdr() == 0) && (deviceId != CPUDEVICE) && !m_nccl->IsSupported()) // non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU
        {
            cudaMemcpy(m_intermediateDistributedCPUBuffer1.get(), distributedMatrixBuffer, count * sizeof(ElemType), cudaMemcpyDeviceToHost);
            m_mpi->AllReduce(m_intermediateDistributedCPUBuffer1.get(), count, op);
            cudaMemcpy(distributedMatrixBuffer, m_intermediateDistributedCPUBuffer1.get(), count * sizeof(ElemType), cudaMemcpyHostToDevice);
        }
        else if (!m_nccl->IsSupported()) // non-NCCL, using CPU, using GDR
        {
            if (m_mpi->UseGpuGdr() == 0) // CPU
            {
                m_mpi->Iallreduce(MPI_IN_PLACE, distributedMatrixBuffer, count,
                                  MPIWrapper::GetDataType(distributedMatrixBuffer), op, &allReduceRequest) ||
                    MpiFail("MPI_Iallreduce");
            }
            else if (deviceId != CPUDEVICE) // GDR && GPU
            {
                m_mpi->AllReduce(distributedMatrixBuffer, count);
            }
            else
                LogicError("LogicError in SimpleDistGradAggregator::DistributedAllReduce");
        }
        else if (m_nccl->IsSupported()) // NCCL
        {
            m_nccl->AllReduce(distributedMatrixBuffer, distributedMatrixBuffer, count, op);
        }
        else
            LogicError("LogicError in SimpleDistGradAggregator::DistributedAllReduce");

        if (m_nccl->IsSupported()) // Non-GDR && GPU
        {
            m_nccl->Sync();
        }
        else if ((m_mpi->UseGpuGdr() == 0) && (deviceId != CPUDEVICE))
        {
        }
        else if (m_mpi->UseGpuGdr() == 0) // CPU
        {
            m_mpi->Wait(&allReduceRequest, MPI_STATUSES_IGNORE) || MpiFail("MPI_Wait"); // Wait for the Iallreduce operations to finish
        }
        else
            LogicError("LogicError in SimpleDistGradAggregator::DistributedAllReduce");
    }

private:
    std::unique_ptr<CUDAPageLockedMemAllocator> m_asyncAllocator;
    std::vector<std::unique_ptr<GPUDataTransferer>> m_asyncGpuDataTransferers;
    std::vector<std::shared_ptr<ElemType>> m_ayncIntermediateCPUBuffers;

private:
    std::unique_ptr<CUDAPageLockedMemAllocator> m_allocator;

    std::vector<std::shared_ptr<ElemType>> m_intermediateCPUBuffers;
    std::vector<std::unique_ptr<GPUDataTransferer>> m_gpuDataTransferers;

    std::shared_ptr<ElemType> m_intermediateDistributedCPUBuffer1;
    std::shared_ptr<ElemType> m_intermediateDistributedCPUBuffer2;

    std::vector<DistGradHeader*> m_recvHeaders;

    // Perform aysnchronous gradient aggregation using double buffering of the gradient matrices
    bool m_useAsyncAggregation;

    // Future corresponding to the current in-flight async gradient aggregation
    std::future<void> m_pendingAsyncAggregation;

    // Buffered gradients that we asynchronously aggregate
    std::unordered_map<Matrix<ElemType>*, std::unique_ptr<Matrix<ElemType>>> m_bufferedGradients;
    DistGradHeader* m_bufferedGradHeader;

    // Packing small gradients (size not larger than threshold size) into a continous buffer to reduce MPI calls.
    // Threshold size to pack a gradient into the continous buffer, default 32KB (tunable by define "packThresholdSizeInKB=[value]")
    const size_t m_packThresholdSizeInBytes;
    std::unique_ptr<Matrix<ElemType>> m_aggregationBuffer;
    std::vector<size_t> m_packedGradientsIndex;
    std::vector<size_t> m_gradientIndexToAggregate;

    int m_syncStatsTrace;

    // Only used for controlling frequency of measuring/showing gradient aggregation perf stats
    size_t m_iterationCount;

    bool m_initialized;

    std::unique_ptr<NcclComm> m_nccl;
};
} // namespace CNTK
} // namespace MSR
} // namespace Microsoft
