#pragma once
#include <atomic>
#include <cstddef>

namespace Allocator {

struct alignas(64) ContextStats
{
    std::atomic<size_t> BytesAllocated{0};
    std::atomic<size_t> BytesFreed{0};
    std::atomic<size_t> AllocationCount{0};
    std::atomic<size_t> PeakUsage{0};

    char _pad[32];

    struct Snapshot
    {
        size_t BytesAllocated;
        size_t BytesFreed;
        size_t AllocationCount;
        size_t PeakUsage;
    };

    Snapshot GetSnapshot() const noexcept
    {
        return Snapshot{BytesAllocated.load(std::memory_order_relaxed),
                        BytesFreed.load(std::memory_order_relaxed),
                        AllocationCount.load(std::memory_order_relaxed),
                        PeakUsage.load(std::memory_order_relaxed)};
    }
};

} // namespace Allocator
