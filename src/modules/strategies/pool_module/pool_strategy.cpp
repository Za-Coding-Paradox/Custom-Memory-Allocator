#include <modules/strategies/pool_module/pool_strategy.h>

namespace Allocator {

static constexpr uintptr_t g_EndOfFreeListMarker = 0;

void PoolStrategy::Format(SlabDescriptor& SlabToInitialize, size_t ChunkSize) noexcept
{
    const uintptr_t SlabStart = SlabToInitialize.GetSlabStart();
    const size_t SlabSize = SlabToInitialize.GetAvailableMemorySize();
    const uintptr_t SlabEnd = SlabStart + static_cast<uintptr_t>(SlabSize);

    SlabToInitialize.UpdateFreeListHead(SlabStart);

    uintptr_t CurrentBlock = SlabStart;
    const size_t TotalSlots = SlabSize / ChunkSize;

    for (size_t i = 0; i < TotalSlots - 1; ++i) {
        uintptr_t NextBlock = CurrentBlock + static_cast<uintptr_t>(ChunkSize);
        *reinterpret_cast<uintptr_t*>(CurrentBlock) = NextBlock;
        CurrentBlock = NextBlock;
    }

    *reinterpret_cast<uintptr_t*>(CurrentBlock) = g_EndOfFreeListMarker;

    SlabToInitialize.SetTotalSlots(TotalSlots);
    SlabToInitialize.SetActiveSlots(0);
}

void PoolStrategy::Free(SlabDescriptor& SlabToFree, void* MemoryToFree) noexcept
{
    if (MemoryToFree == nullptr) [[unlikely]]
        return;

    auto& Head = SlabToFree.GetFreeListHeadAtomic();
    const uintptr_t Block = reinterpret_cast<uintptr_t>(MemoryToFree);

    uintptr_t Current = Head.load(std::memory_order_relaxed);
    do {
        *reinterpret_cast<uintptr_t*>(Block) = Current;
    } while (!Head.compare_exchange_weak(Current, Block, std::memory_order_release,
                                         std::memory_order_relaxed));

    ALLOCATOR_DIAGNOSTIC({
        const size_t CurrentActive = SlabToFree.GetActiveSlots();
        if (CurrentActive > 0) [[likely]] {
            SlabToFree.SetActiveSlots(CurrentActive - 1);
        }
    });
}

} // namespace Allocator
