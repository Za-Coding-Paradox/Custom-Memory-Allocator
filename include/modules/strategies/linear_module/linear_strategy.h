#pragma once

#include <modules/strategies/linear_module/linear_scopes.h>

namespace Allocator {

static constexpr size_t g_LinearStrategyAlignment = 16;

class LinearStrategy
{
public:
    LinearStrategy() = delete;
    ~LinearStrategy() = delete;

    [[nodiscard]] __attribute__((always_inline)) static inline bool
    CanFit(const SlabDescriptor& AllocationSlab, size_t AllocationSize,
           size_t Alignment = g_LinearStrategyAlignment) noexcept
    {
        const uintptr_t CurrentHead = AllocationSlab.GetFreeListHead();

        const uintptr_t AlignedStart = reinterpret_cast<uintptr_t>(
            Utility::AlignForward(reinterpret_cast<void*>(CurrentHead), Alignment));

        return (AlignedStart + AllocationSize) <= (AllocationSlab.GetSlabStart() + g_ConstSlabSize);
    }

    [[nodiscard]] __attribute__((always_inline)) static inline void*
    Allocate(SlabDescriptor& AllocationSlab, size_t AllocationSize,
             size_t Alignment = g_LinearStrategyAlignment) noexcept
    {
        const uintptr_t CurrentHead = AllocationSlab.GetFreeListHead();

        const uintptr_t AlignedDataStart = reinterpret_cast<uintptr_t>(
            Utility::AlignForward(reinterpret_cast<void*>(CurrentHead), Alignment));

        const uintptr_t NewFreeListHead = AlignedDataStart + static_cast<uintptr_t>(AllocationSize);
        const uintptr_t SlabEnd = AllocationSlab.GetSlabStart() + g_ConstSlabSize;

        if (NewFreeListHead > SlabEnd) [[unlikely]] {
            return nullptr;
        }

        AllocationSlab.UpdateFreeListHead(NewFreeListHead);

        ALLOCATOR_DIAGNOSTIC({ AllocationSlab.IncrementActiveSlots(); });

        return reinterpret_cast<void*>(AlignedDataStart);
    }

    static inline void Free(SlabDescriptor&, void*) noexcept { /* No-Op by design */ }

    static void Reset(SlabDescriptor& SlabToReset) noexcept;

    static void RewindToMarker(SlabDescriptor& SlabToRewind, uintptr_t RewindOffset) noexcept;
};

} // namespace Allocator
