#include <modules/strategies/linear_module/linear_strategy.h>

namespace Allocator {

void LinearStrategy::Reset(SlabDescriptor& SlabToReset) noexcept
{
    LOG_ALLOCATOR("DEBUG", "LinearStrategy: Resetting Slab");

    const uintptr_t Start = SlabToReset.GetSlabStart();
    SlabToReset.UpdateFreeListHead(Start);

    ALLOCATOR_DIAGNOSTIC({ SlabToReset.ResetSlab(); });
}

void LinearStrategy::RewindToMarker(SlabDescriptor& SlabToRewind, uintptr_t RewindOffset) noexcept
{
    ALLOCATOR_ASSERT(RewindOffset >= SlabToRewind.GetSlabStart(), "Rewind before Slab Start");
    ALLOCATOR_ASSERT(RewindOffset <= (SlabToRewind.GetSlabStart() + g_ConstSlabSize),
                     "Rewind past Slab End");

    SlabToRewind.UpdateFreeListHead(RewindOffset);
}

} // namespace Allocator
