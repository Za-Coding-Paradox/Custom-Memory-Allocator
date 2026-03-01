#include <modules/strategies/linear_module/linear_strategy.h>

namespace Allocator {

void LinearStrategy::Reset(SlabDescriptor& SlabToReset) noexcept
{
    LOG_ALLOCATOR("DEBUG", "LinearStrategy: Reset slab at 0x"
                               << std::hex << SlabToReset.GetSlabStart() << std::dec);

    SlabToReset.UpdateFreeListHead(SlabToReset.GetSlabStart());

    ALLOCATOR_DIAGNOSTIC({ SlabToReset.SetActiveSlots(0); });
}

void LinearStrategy::RewindToMarker(SlabDescriptor& SlabToRewind, uintptr_t RewindOffset) noexcept
{
    ALLOCATOR_ASSERT(RewindOffset >= SlabToRewind.GetSlabStart(),
                     "RewindToMarker: Offset is before SlabStart — marker was not captured "
                     "on this slab or memory was corrupted.");
    ALLOCATOR_ASSERT(RewindOffset <= (SlabToRewind.GetSlabStart() + g_ConstSlabSize),
                     "RewindToMarker: Offset is past SlabEnd — marker is invalid.");

    SlabToRewind.UpdateFreeListHead(RewindOffset);
}

} // namespace Allocator
