#pragma once

#include "DistGradHeader.h"
#include "MPIWrapper.h"

namespace Microsoft
{
namespace MSR
{
namespace CNTK
{

template <class ElemType>
class IDistGradAggregator
{
public:
    IDistGradAggregator(const MPIWrapperPtr& mpi)
        : m_mpi(mpi)
    {
    }

    virtual ~IDistGradAggregator()
    {
    }

    // Returns a boolean indicating if any samples were processed
    virtual bool AggregateGradients(const std::vector<Matrix<ElemType>*>& gradients, DistGradHeader* headerCPU, bool resetState) = 0;

    virtual bool DistributedCheck(size_t minibatchSize, size_t processNum) = 0;

    virtual void DistributedInit(DEVICEID_TYPE deviceId, size_t bufferSize) = 0;

    virtual void DistributedAllGather(const Matrix<ElemType>& distributedMatrix, Matrix<ElemType>& gatheredMatrix, size_t count) = 0;

    virtual void DistributedAllReduce(const Matrix<ElemType>& distributedMatrix, MPI_Op op) = 0;

    size_t NumProc()
    {
        return m_mpi->NumNodesInUse();
    }

    size_t MyRank()
    {
        return m_mpi->CurrentNodeRank();
    }

    void WaitAll()
    {
        m_mpi->WaitAll();
    }

public:
    virtual bool AsyncAggregateGradHeader(DistGradHeader* headerCPU){return headerCPU == nullptr;}

protected:
    MPIWrapperPtr m_mpi;
};

#define UsingIDistGradAggregatorMembers           \
                                                  \
protected:                                        \
    using IDistGradAggregator<ElemType>::m_mpi;   \
    using IDistGradAggregator<ElemType>::NumProc; \
    using IDistGradAggregator<ElemType>::MyRank
} // namespace CNTK
} // namespace MSR
} // namespace Microsoft
