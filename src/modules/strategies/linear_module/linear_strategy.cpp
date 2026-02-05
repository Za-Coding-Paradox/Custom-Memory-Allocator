#include <modules/strategies/linear_module/linear_strategy.h>

namespace Allocator {

bool LinearStrategy::CanFit(const SlabDescriptor& AllocationSlab, size_t AllocationSize,
                            size_t Alignment) noexcept {

  const uintptr_t CurrentHead = AllocationSlab.GetFreeListHead();
  const uintptr_t SlabStart = AllocationSlab.GetSlabStart();

  const size_t SlabSize = g_ConstSlabSize;

  const uintptr_t AlignedStart =
      std::bit_cast<uintptr_t>(Utility::AlignForward(std::bit_cast<void*>(CurrentHead), Alignment));

  const uintptr_t NewHead = AlignedStart + static_cast<uintptr_t>(AllocationSize);
  const uintptr_t SlabEnd = SlabStart + static_cast<uintptr_t>(SlabSize);

  bool BFits = (NewHead <= SlabEnd);

  return BFits;
}

void* LinearStrategy::Allocate(SlabDescriptor& AllocationSlab, size_t AllocationSize,
                               size_t Alignment) noexcept {

  const uintptr_t CurrentHead = AllocationSlab.GetFreeListHead();

  const uintptr_t AlignedDataStart =
      std::bit_cast<uintptr_t>(Utility::AlignForward(std::bit_cast<void*>(CurrentHead), Alignment));

  const uintptr_t NewFreeListHead = AlignedDataStart + static_cast<uintptr_t>(AllocationSize);

  const uintptr_t SlabStart = AllocationSlab.GetSlabStart();
  const size_t SlabSize = g_ConstSlabSize;
  const uintptr_t SlabEnd = SlabStart + static_cast<uintptr_t>(SlabSize);

  if (NewFreeListHead > SlabEnd) [[unlikely]] {
    LOG_ALLOCATOR("ERROR", "LinearStrategy: Allocation Failed - Size: "
                               << AllocationSize << " Alignment: " << Alignment
                               << " Remaining: " << (SlabEnd - CurrentHead));
    return nullptr;
  }

  // 6. Commit
  AllocationSlab.UpdateFreeListHead(NewFreeListHead);
  AllocationSlab.IncrementActiveSlots();

  return std::bit_cast<void*>(AlignedDataStart);
}

void LinearStrategy::Free(SlabDescriptor& SlabToFree, void* MemoryAddress) noexcept {
  (void)SlabToFree;
  (void)MemoryAddress;

  LOG_ALLOCATOR("WARN", "LinearStrategy: Free called - This is a no-op.");
}

void LinearStrategy::Reset(SlabDescriptor& SlabToReset) noexcept {
  LOG_ALLOCATOR("DEBUG", "LinearStrategy: Resetting Slab - Address: "
                             << std::bit_cast<void*>(SlabToReset.GetSlabStart()));
  SlabToReset.UpdateFreeListHead(SlabToReset.GetSlabStart());
  SlabToReset.ResetSlab();
}

void LinearStrategy::RewindToMarker(SlabDescriptor& SlabToRewind, uintptr_t RewindOffset) noexcept {

  LOG_ALLOCATOR("DEBUG", "LinearStrategy: Rewinding Slab to Offset: " << RewindOffset);

  const uintptr_t SlabStart = SlabToRewind.GetSlabStart();
  const size_t SlabSize = g_ConstSlabSize;
  const uintptr_t SlabEnd = SlabStart + static_cast<uintptr_t>(SlabSize);

  if (RewindOffset < SlabStart) [[unlikely]] {
    LOG_ALLOCATOR("CRITICAL", "LinearStrategy: Rewind offset before slab start! Offset: "
                                  << RewindOffset << " Start: " << SlabStart);
    return;
  }

  if (RewindOffset > SlabEnd) [[unlikely]] {
    LOG_ALLOCATOR("CRITICAL", "LinearStrategy: Rewind offset beyond slab end! Offset: "
                                  << RewindOffset << " End: " << SlabEnd);
    return;
  }

  SlabToRewind.UpdateFreeListHead(RewindOffset);
}

} // namespace Allocator
