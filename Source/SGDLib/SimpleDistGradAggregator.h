//
// Copyright (c) Microsoft. All rights reserved.
// Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#pragma once

#define __PROFILE__

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

namespace Microsoft
{
namespace MSR
{
namespace CNTK
{

#ifdef __PROFILE__
static size_t profileCnt = 0;
#endif

#ifdef __PROFILE__
std::chrono::time_point<std::chrono::system_clock> sdStartTime;
std::chrono::time_point<std::chrono::system_clock> sdEndTime;

double sdCudaMemcpyAndMPIAllGatherTime = 0.0;
double sdMPIIallgatherTime = 0.0;
double sdMPIAllGatherTime = 0.0;
double sdNCCLAllGatherTime = 0.0;
double sdNCCLSyncTime = 0.0;
double sdMPIWaitTime = 0.0;

double aggAsyncTime = 0.0;
double aggSwapTime = 0.0;
double aggCopyGradDataToBufferTime = 0.0;
double aggInitRecvHeaderAndSendNodes = 0.0;
double aggNCCLAllReduceTime = 0.0;
double aggMainNodeWaitAndAggTime = 0.0;
double aggMPIBcastTime = 0.0;
double aggNCCLSyncTime = 0.0;
double aggCopyDataBackToGradTime = 0.0;
double aggMPIWaitTime = 0.0;
#endif

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
#ifdef __PROFILE__
            if (profileCnt % 100 == 0)
                LOGPRINTF(stderr, "Use Async Aggregation\n");
#endif

#ifdef __PROFILE__
            sdStartTime = std::chrono::system_clock::now();
#endif
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
#ifdef __PROFILE__
            sdEndTime = std::chrono::system_clock::now();
            aggAsyncTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

			if (profileCnt % 100 == 0)
            {
                fprintf(stderr, "Iteration [%d]: Async gradient aggregation wait time time = %.8gs\n", (int) profileCnt, aggAsyncTime);
                aggAsyncTime = 0.0;
            }
#endif

#ifdef __PROFILE__
                sdStartTime = std::chrono::system_clock::now();
#endif
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
#ifdef __PROFILE__
            sdEndTime = std::chrono::system_clock::now();
            aggSwapTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

			if (profileCnt % 100 == 0)
            {
                fprintf(stderr, "Iteration [%d]: Swap the grad header contents with the buffered grad header time = %.8gs\n", (int) profileCnt, aggSwapTime);
                aggSwapTime = 0.0;
            }
#endif

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

#ifdef __PROFILE__
        sdStartTime = std::chrono::system_clock::now();
#endif  // __PROFILE__
        // Copy all gradient data into a single contiguous buffer, if additional continous buffer allocated
        size_t offset = 0;
        for (size_t i : m_packedGradientsIndex)
        {
            m_aggregationBuffer->ColumnSlice(offset, gradients[i]->GetNumElements()).AssignValuesOf(gradients[i]->Reshaped(1, gradients[i]->GetNumElements()));
            offset += gradients[i]->GetNumElements();
        }
#ifdef __PROFILE__
        sdEndTime = std::chrono::system_clock::now();
        aggCopyGradDataToBufferTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

		if (profileCnt % 100 == 0)
        {
            fprintf(stderr, "Iteration [%d]: Copy all gradient data into a single contiguous buffer time = %.8gs\n", (int) profileCnt, aggCopyGradDataToBufferTime);
            aggCopyGradDataToBufferTime = 0.0;
        }

#endif // __PROFILE__

#ifdef __PROFILE__
        sdStartTime = std::chrono::system_clock::now();
#endif  // __PROFILE__
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
#ifdef __PROFILE__
        sdEndTime = std::chrono::system_clock::now();
        aggInitRecvHeaderAndSendNodes += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

		if (profileCnt % 100 == 0)
        {
            fprintf(stderr, "Iteration [%d]: Initiate receive of the header on the main node time = %.8gs\n", (int) profileCnt, aggInitRecvHeaderAndSendNodes);
            aggInitRecvHeaderAndSendNodes = 0.0;
        }
#endif // __PROFILE__

        // New aggregation pipeline for non-GDR, perform sync allreduce on the gradient data
        // For CPU, still use async allreduce
        std::vector<MPI_Request> allReduceRequests;
        size_t gpuToCpuIndex = 0;
        size_t cpuToGpuIndex = 0;
        size_t allReduceIndex = 0;
        size_t numGradientIndex = m_gradientIndexToAggregate.size();
        if (numGradientIndex > 0)
        {
#ifdef __PROFILE__
            if (profileCnt % 100 == 0)
            {
                LOGPRINTF(stderr, "m_mpi->UseGpuGdr() = %d\n", m_mpi->UseGpuGdr());
                LOGPRINTF(stderr, "deviceId = %d\n", deviceId);
                LOGPRINTF(stderr, "m_nccl->IsSupported() = %d\n", m_nccl->IsSupported());
            }
#endif
            // non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU
            if ((m_mpi->UseGpuGdr() == 0) && (deviceId != CPUDEVICE) && !m_nccl->IsSupported())
            {
#ifdef __PROFILE__
                if (profileCnt++ % 100 == 0)
                    LOGPRINTF(stderr, "AggregateGradientsImpl Branch1[non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU] : m_mpi->UseGpuGdr() == false && deviceId != CPUDEVICE && m_nccl->IsSupported() == false \n");
#endif
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
#ifdef __PROFILE__
                if (profileCnt++ % 100 == 0)
                    LOGPRINTF(stderr, "AggregateGradientsImpl Branch2[non-NCCL, using CPU, using GDR] : m_nccl->IsSupported() == false \n");
#endif
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
#ifdef __PROFILE__
                if (profileCnt++ % 100 == 0)
                    LOGPRINTF(stderr, "AggregateGradientsImpl Branch3 : m_nccl->IsSupported() == true \n");
#endif

#ifdef __PROFILE__
                sdStartTime = std::chrono::system_clock::now();
#endif // __PROFILE__
                std::vector<Matrix<ElemType>*> ncclReduceGradients;
                for (size_t i : m_gradientIndexToAggregate)
                {
                    ncclReduceGradients.push_back((i == -1) ? m_aggregationBuffer.get() : gradients[i]);
                }
                m_nccl->AllReduce(ncclReduceGradients);
#ifdef __PROFILE__
                sdEndTime = std::chrono::system_clock::now();
                aggNCCLAllReduceTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

				if ((profileCnt - 1) % 100 == 0)
                {
                    fprintf(stderr, "Iteration [%d]: aggregate nccl allreduce time = %.8gs\n", (int) profileCnt, aggNCCLAllReduceTime);
                    aggNCCLAllReduceTime = 0.0;
                }
#endif
            }
        }

#ifdef __PROFILE__
        sdStartTime = std::chrono::system_clock::now();
#endif // __PROFILE__ \
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
#ifdef __PROFILE__
        sdEndTime = std::chrono::system_clock::now();
        aggMainNodeWaitAndAggTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

		if ((profileCnt - 1) % 100 == 0)
        {
            fprintf(stderr, "Iteration [%d]: On the main node wait for the headers to arrive and aggregate time = %.8gs\n", (int) profileCnt, aggMainNodeWaitAndAggTime);
            aggMainNodeWaitAndAggTime = 0.0;
        }
#endif

#ifdef __PROFILE__
        sdStartTime = std::chrono::system_clock::now();
#endif // __PROFILE__ \
    // Broadcast the aggregated header to all nodes
        m_mpi->Bcast(headerCPU, headerCPU->Size(), MPI_CHAR, m_mpi->MainNodeRank());
#ifdef __PROFILE__
        sdEndTime = std::chrono::system_clock::now();
        aggMPIBcastTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

		if ((profileCnt - 1) % 100 == 0)
        {
            fprintf(stderr, "Iteration [%d]: Broadcast the aggregated header to all nodes time = %.8gs\n", (int) profileCnt, aggMPIBcastTime);
            aggMPIBcastTime = 0.0;
        }
#endif

#ifdef __PROFILE__
        sdStartTime = std::chrono::system_clock::now();
#endif // __PROFILE__
        if (m_nccl->IsSupported())
        {
            m_nccl->Sync();
#ifdef __PROFILE__
            sdEndTime = std::chrono::system_clock::now();
            aggNCCLSyncTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

			if ((profileCnt - 1) % 100 == 0)
            {
                fprintf(stderr, "Iteration [%d]: aggregate nccl sync time = %.8gs\n", (int) profileCnt, aggNCCLSyncTime);
                aggNCCLSyncTime = 0.0;
            }
#endif
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

#ifdef __PROFILE__
        sdStartTime = std::chrono::system_clock::now();
#endif // __PROFILE__ \
    // Copy data back to the packed gradients from the continous buffer
        offset = 0;
        for (size_t i : m_packedGradientsIndex)
        {
            gradients[i]->AssignValuesOf(m_aggregationBuffer->ColumnSlice(offset, gradients[i]->GetNumElements()).Reshaped(gradients[i]->GetNumRows(), gradients[i]->GetNumCols()));
            offset += gradients[i]->GetNumElements();
        }
#ifdef __PROFILE__
        sdEndTime = std::chrono::system_clock::now();
        aggCopyDataBackToGradTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

		if ((profileCnt - 1) % 100 == 0)
        {
            fprintf(stderr, "Iteration [%d]: Copy data back to the packed gradients from the continous buffer time = %.8gs\n", (int) profileCnt, aggCopyDataBackToGradTime);
            aggCopyDataBackToGradTime = 0.0;
        }
#endif

#ifdef __PROFILE__
        sdStartTime = std::chrono::system_clock::now();
#endif // __PROFILE__ \
    // Wait for completion of the async send requests
        if (!m_mpi->IsMainNode())
            m_mpi->Wait(&sendHeaderRequest, MPI_STATUSES_IGNORE) || MpiFail("MPI_Wait");
#ifdef __PROFILE__
        sdEndTime = std::chrono::system_clock::now();
        aggMPIWaitTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

		if ((profileCnt - 1) % 100 == 0)
        {
            fprintf(stderr, "Iteration [%d]: Wait for completion of the async send requests time = %.8gs\n", (int) profileCnt, aggMPIWaitTime);
            aggMPIWaitTime = 0.0;
        }
#endif

        if (showSyncPerfStats)
        {
            aggregationTimer.Stop();
            double gradientAggregationTime = aggregationTimer.ElapsedSeconds();
            fprintf(stderr, "Actual gradient aggregation time: %.6g\n", gradientAggregationTime);
        }
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
        }
    }

    void DistributedAllGather(const Matrix<ElemType>& distributedMatrix, Matrix<ElemType>& gatheredMatrix, size_t count)
    {
        int deviceId = distributedMatrix.GetDeviceId();
        MPI_Request allGatherRequest;
        ElemType* distributedMatrixBuffer = distributedMatrix.Data();
        ElemType* gatheredMatrixBuffer = gatheredMatrix.Data();

#ifdef __PROFILE__
        if (profileCnt % 100 == 0)
            LOGPRINTF(stderr, "DistributedMatrixBuffer Shape: rows: %zd; cols: %zd\n", distributedMatrix.GetNumRows(), distributedMatrix.GetNumCols());
			LOGPRINTF(stderr, "GatheredMatrixBuffe Shape: rows: %zd; cols: %zd\n", gatheredMatrix.GetNumRows(), gatheredMatrix.GetNumCols());

#endif // __PROFILE__

        if ((m_mpi->UseGpuGdr() == 0) && (deviceId != CPUDEVICE) && !m_nccl->IsSupported()) // non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU
        {
#ifdef __PROFILE__
            if (profileCnt % 100 == 0)
                LOGPRINTF(stderr, "DistributedAllGather Branch1[non-GDR && GPU && non-NCCL: need to copy data from GPU to CPU] : m_mpi->UseGpuGdr() == false && deviceId != CPUDEVICE && m_nccl->IsSupported() == false \n");
            sdStartTime = std::chrono::system_clock::now();
#endif
            cudaMemcpy(m_intermediateDistributedCPUBuffer1.get(), distributedMatrixBuffer, count * sizeof(ElemType), cudaMemcpyDeviceToHost);
            m_mpi->AllGather(m_intermediateDistributedCPUBuffer1.get(), count, m_intermediateDistributedCPUBuffer2.get(), count);
            cudaMemcpy(gatheredMatrixBuffer, m_intermediateDistributedCPUBuffer2.get(), gatheredMatrix.GetNumElements() * sizeof(ElemType), cudaMemcpyHostToDevice);
#ifdef __PROFILE__
            sdEndTime = std::chrono::system_clock::now();
            sdCudaMemcpyAndMPIAllGatherTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

            if (profileCnt % 100 == 0)
            {
                fprintf(stderr, "Iteration [%d]: cuda memcpy and mpi allgather time = %.8gs\n", (int) profileCnt, sdCudaMemcpyAndMPIAllGatherTime);
                sdCudaMemcpyAndMPIAllGatherTime = 0.0;
            }
#endif
        }
        else if (!m_nccl->IsSupported()) // non-NCCL, using CPU, using GDR
        {
#ifdef __PROFILE__
            if (profileCnt % 100 == 0)
                LOGPRINTF(stderr, "DistributedAllGather Branch2[non-NCCL, using CPU, using GDR] : m_nccl->IsSupported() == false \n");
            sdStartTime = std::chrono::system_clock::now();
#endif
            if (m_mpi->UseGpuGdr() == 0) // CPU
            {
#ifdef __PROFILE__
                if (profileCnt % 100 == 0)
                    LOGPRINTF(stderr, "DistributedAllGather Branch2.1[non-NCCL, using CPU]\n");
#endif
                m_mpi->Iallgather(distributedMatrixBuffer, gatheredMatrixBuffer, count,
                                  MPIWrapper::GetDataType(distributedMatrixBuffer), &allGatherRequest) ||
                    MpiFail("MPI_Iallgather");
#ifdef __PROFILE__
                sdEndTime = std::chrono::system_clock::now();
                sdMPIIallgatherTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

                if (profileCnt % 100 == 0)
                {
                    fprintf(stderr, "Iteration [%d]: mpi iallgather time = %.8gs\n", (int) profileCnt, sdMPIIallgatherTime);
                    sdMPIIallgatherTime = 0.0;
                }
#endif
            }
            else if (deviceId != CPUDEVICE) // GDR && GPU
            {
#ifdef __PROFILE__
                if (profileCnt % 100 == 0)
                    LOGPRINTF(stderr, "DistributedAllGather Branch2.2[non-NCCL, using GPU&GDR]\n");
#endif
                m_mpi->AllGather(distributedMatrixBuffer, count, gatheredMatrixBuffer, count);
#ifdef __PROFILE__
                sdEndTime = std::chrono::system_clock::now();
                sdMPIAllGatherTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

                if (profileCnt % 100 == 0)
                {
                    fprintf(stderr, "Iteration [%d]: mpi allgather time = %.8gs\n", (int) profileCnt, sdMPIAllGatherTime);
                    sdMPIAllGatherTime = 0.0;
                }
#endif
            }
            else
                LogicError("LogicError in SimpleDistGradAggregator::DistributedAllGather");
        }
        else if (m_nccl->IsSupported()) // NCCL
        {
#ifdef __PROFILE__
            if (profileCnt % 100 == 0)
                LOGPRINTF(stderr, "DistributedAllGather Branch3 : m_nccl->IsSupported() == true \n");
            sdStartTime = std::chrono::system_clock::now();
#endif
            m_nccl->AllGather(distributedMatrixBuffer, gatheredMatrixBuffer, count);
#ifdef __PROFILE__
            sdEndTime = std::chrono::system_clock::now();
            sdNCCLAllGatherTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

            if (profileCnt % 100 == 0)
            {
                fprintf(stderr, "Iteration [%d]: nccl allgather time = %.8gs\n", (int) profileCnt, sdNCCLAllGatherTime);
                sdNCCLAllGatherTime = 0.0;
            }
#endif
        }
        else
            LogicError("LogicError in SimpleDistGradAggregator::DistributedAllGather");

#ifdef __PROFILE__
        sdStartTime = std::chrono::system_clock::now();
#endif                             // __PROFILE__
        if (m_nccl->IsSupported()) // Non-GDR && GPU
        {
            m_nccl->Sync();
#ifdef __PROFILE__
            sdEndTime = std::chrono::system_clock::now();
            sdNCCLSyncTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

            if (profileCnt % 100 == 0)
            {
                fprintf(stderr, "Iteration [%d]: nccl sync time = %.8gs\n", (int) profileCnt, sdNCCLSyncTime);
                sdNCCLSyncTime = 0.0;
            }
#endif
        }
        else if ((m_mpi->UseGpuGdr() == 0) && (deviceId != CPUDEVICE))
        {
        }
        else if (m_mpi->UseGpuGdr() == 0) // CPU
        {
            m_mpi->Wait(&allGatherRequest, MPI_STATUSES_IGNORE) || MpiFail("MPI_Wait"); // Wait for the Iallreduce operations to finish
#ifdef __PROFILE__
            sdEndTime = std::chrono::system_clock::now();
            sdMPIWaitTime += (std::chrono::duration<double>(sdEndTime - sdStartTime)).count();

            if (profileCnt % 100 == 0)
            {
                fprintf(stderr, "Iteration [%d]: mpi wait time = %.8gs\n", (int) profileCnt, sdMPIWaitTime);
                sdMPIWaitTime = 0.0;
            }
#endif
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
