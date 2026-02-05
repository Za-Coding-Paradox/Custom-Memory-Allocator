#pragma once

#include <Core/core.h>

namespace Allocator {

struct ContextStats {
  std::atomic<size_t> BytesAllocated{0};
  std::atomic<size_t> BytesFreed{0};
  std::atomic<size_t> AllocationCount{0};
  std::atomic<size_t> PeakUsage{0};

  struct Snapshot {
    size_t Allocated;
    size_t Freed;
    size_t Count;
    size_t Peak;
    size_t Current;
  };

  [[nodiscard]] Snapshot GetSnapshot() const noexcept {
    size_t alloc = BytesAllocated.load(std::memory_order_relaxed);
    size_t freed = BytesFreed.load(std::memory_order_relaxed);
    return Snapshot{.Allocated = alloc,
                    .Freed = freed,
                    .Count = AllocationCount.load(std::memory_order_relaxed),
                    .Peak = PeakUsage.load(std::memory_order_relaxed),
                    .Current = (alloc > freed) ? (alloc - freed) : 0};
  }
};

} // namespace Allocator
