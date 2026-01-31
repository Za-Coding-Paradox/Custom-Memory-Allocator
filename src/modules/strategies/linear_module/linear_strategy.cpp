#include <modules/strategies/linear_module/linear_strategy.h>

namespace Allocator {

bool LinearStrategy::CanFit(const SlabDescriptor &AllocationSlab,
                            size_t AllocationSize, size_t Alignment) noexcept {

  const uintptr_t CurrentHead = AllocationSlab.GetFreeListHead();
  const uintptr_t SlabStart = AllocationSlab.GetSlabStart();

  // FIX: Using global constant since Registry getters are not static
  const size_t SlabSize = g_ConstSlabSize;

  // 1. Calculate aligned start position
  // We must align the CurrentHead UP to the nearest multiple of 'Alignment'
  const uintptr_t AlignedStart = std::bit_cast<uintptr_t>(
      Utility::AlignForward(std::bit_cast<void *>(CurrentHead), Alignment));

  // 2. Calculate where the data would end
  const uintptr_t NewHead =
      AlignedStart + static_cast<uintptr_t>(AllocationSize);
  const uintptr_t SlabEnd = SlabStart + static_cast<uintptr_t>(SlabSize);

  bool BFits = (NewHead <= SlabEnd);

  // LOG_ALLOCATOR("TRACE", "CanFit Check - Size: " << AllocationSize << " -
  // Fits: " << BFits);

  return BFits;
}

void *LinearStrategy::Allocate(SlabDescriptor &AllocationSlab,
                               size_t AllocationSize,
                               size_t Alignment) noexcept {

  const uintptr_t CurrentHead = AllocationSlab.GetFreeListHead();
  const uintptr_t SlabStart = AllocationSlab.GetSlabStart();

  // 1. Calculate the ALIGNED start address for this new object
  //    (Bump 'CurrentHead' forward if it's not aligned properly)
  const uintptr_t AlignedDataStart = std::bit_cast<uintptr_t>(
      Utility::AlignForward(std::bit_cast<void *>(CurrentHead), Alignment));

  // 2. Calculate the NEW free list head (End of this allocation)
  const uintptr_t NewFreeListHead =
      AlignedDataStart + static_cast<uintptr_t>(AllocationSize);

  const size_t SlabSize = g_ConstSlabSize;
  const uintptr_t SlabEnd = SlabStart + static_cast<uintptr_t>(SlabSize);

  // 3. Check bounds
  if (NewFreeListHead > SlabEnd)
      [[unlikely]] { // FIX (Bug #20): Added [[unlikely]]
    LOG_ALLOCATOR("ERROR", "LinearStrategy: Allocation Failed - Size: "
                               << AllocationSize << " Alignment: " << Alignment
                               << " Remaining: " << (SlabEnd - CurrentHead));
    return nullptr;
  }

  // 4. Update the free list pointer
  AllocationSlab.UpdateFreeListHead(NewFreeListHead);

  LOG_ALLOCATOR("DEBUG", "LinearStrategy: Allocated - "
                             << AllocationSize << " bytes @ "
                             << std::bit_cast<void *>(AlignedDataStart));

  // 5. Return the properly ALIGNED pointer
  return std::bit_cast<void *>(AlignedDataStart);
}

void LinearStrategy::Free(SlabDescriptor &SlabToFree,
                          void *MemoryAddress) noexcept {
  (void)SlabToFree;
  (void)MemoryAddress;

  LOG_ALLOCATOR("WARN", "LinearStrategy: Free called - This is a no-op.");
}

void LinearStrategy::Reset(SlabDescriptor &SlabToReset) noexcept {
  LOG_ALLOCATOR("DEBUG",
                "LinearStrategy: Resetting Slab - Address: "
                    << std::bit_cast<void *>(SlabToReset.GetSlabStart()));

  SlabToReset.ResetSlab();
}

void LinearStrategy::RewindToMarker(SlabDescriptor &SlabToRewind,
                                    uintptr_t RewindOffset) noexcept {

  LOG_ALLOCATOR("DEBUG",
                "LinearStrategy: Rewinding Slab to Offset: " << RewindOffset);

  const uintptr_t SlabStart = SlabToRewind.GetSlabStart();
  const size_t SlabSize = g_ConstSlabSize;
  const uintptr_t SlabEnd = SlabStart + static_cast<uintptr_t>(SlabSize);

  // FIX (Bug #9): Validate both lower and upper bounds
  if (RewindOffset < SlabStart) [[unlikely]] {
    LOG_ALLOCATOR("CRITICAL",
                  "LinearStrategy: Rewind offset before slab start! Offset: "
                      << RewindOffset << " Start: " << SlabStart);
    return;
  }

  if (RewindOffset > SlabEnd)
      [[unlikely]] { // FIX (Bug #9): Added upper bound check
    LOG_ALLOCATOR("CRITICAL",
                  "LinearStrategy: Rewind offset beyond slab end! Offset: "
                      << RewindOffset << " End: " << SlabEnd);
    return;
  }

  SlabToRewind.UpdateFreeListHead(RewindOffset);
}

} // namespace Allocator
