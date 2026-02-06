#include <modules/strategies/pool_module/pool_strategy.h>

namespace Allocator {

// names so amigous i had to write this explaination, ;P
// is used to check if we have space for atleast two blocks in the slab.
static const size_t g_AvailableBlocksCheckIncrement = 2;
static const size_t g_EndOfFreeListMarkerForNoFreeSpaceSlab = 0;

void PoolStrategy::Format(SlabDescriptor& SlabToInitialize, size_t ChunkSize) noexcept
{
    const uintptr_t SlabStart = SlabToInitialize.GetSlabStart();
    const size_t SlabSize = SlabToInitialize.GetAvailableMemorySize();
    const uintptr_t SlabEnd = SlabStart + static_cast<uintptr_t>(SlabSize);

    SlabToInitialize.UpdateFreeListHead(SlabStart);

    uintptr_t CurrentBlock = SlabStart;

    while (static_cast<uintptr_t>(
               CurrentBlock +
               (static_cast<uintptr_t>(g_AvailableBlocksCheckIncrement) * ChunkSize)) <= SlabEnd) {
        auto* NextLink = std::bit_cast<uintptr_t*>(CurrentBlock);
        *NextLink = CurrentBlock + static_cast<uintptr_t>(ChunkSize);
        CurrentBlock += static_cast<uintptr_t>(ChunkSize);
    }

    auto* LastLink = std::bit_cast<uintptr_t*>(CurrentBlock);
    *LastLink = static_cast<uintptr_t>(g_EndOfFreeListMarkerForNoFreeSpaceSlab);

    const size_t TotalSlots = SlabSize / ChunkSize;
    SlabToInitialize.SetTotalSlots(TotalSlots);
    SlabToInitialize.SetActiveSlots(0);
}

[[nodiscard]] void* PoolStrategy::Allocate(SlabDescriptor& SlabToAllocate) noexcept
{
    const uintptr_t AvailableAddress = SlabToAllocate.GetFreeListHead();

    if (AvailableAddress == static_cast<uintptr_t>(g_EndOfFreeListMarkerForNoFreeSpaceSlab))
        [[unlikely]] {
        return nullptr;
    }

    const uintptr_t NextFreeAddress = *std::bit_cast<uintptr_t*>(AvailableAddress);

    SlabToAllocate.UpdateFreeListHead(NextFreeAddress);
    SlabToAllocate.IncrementActiveSlots();

    return std::bit_cast<void*>(AvailableAddress);
}

void PoolStrategy::Free(SlabDescriptor& SlabToFree, void* MemoryToFree) noexcept
{
    if (MemoryToFree == nullptr) [[unlikely]] {
        return;
    }

    const auto BlockToReturn = std::bit_cast<uintptr_t>(MemoryToFree);
    const uintptr_t OldHead = SlabToFree.GetFreeListHead();

    auto* NextLink = std::bit_cast<uintptr_t*>(BlockToReturn);
    *NextLink = OldHead;

    SlabToFree.UpdateFreeListHead(BlockToReturn);

    const size_t CurrentActive = SlabToFree.GetActiveSlots();
    if (CurrentActive > 0) [[likely]] {
        SlabToFree.SetActiveSlots(CurrentActive - static_cast<size_t>(1));
    }
}

[[nodiscard]] bool PoolStrategy::CanFit(const SlabDescriptor& SlabToCheck) noexcept
{
    return SlabToCheck.GetFreeListHead() !=
           static_cast<uintptr_t>(g_EndOfFreeListMarkerForNoFreeSpaceSlab);
}

} // namespace Allocator
