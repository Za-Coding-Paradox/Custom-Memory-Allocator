#pragma once

#include <modules/strategies/linear_module/linear_scopes.h>

namespace Allocator {

static constexpr size_t g_LinearStrategyAlignment = 16;
static constexpr uintptr_t g_LinearStrategyAlignmentMask = g_LinearStrategyAlignment - 1;

class LinearStrategy
{
public:
    LinearStrategy() = delete;
    ~LinearStrategy() = delete;

    [[nodiscard]] static bool CanFit(const SlabDescriptor& AllocationSlab, size_t AllocationSize,
                                     size_t Alignment = g_LinearStrategyAlignment) noexcept;

    [[nodiscard]] static void* Allocate(SlabDescriptor& AllocationSlab, size_t AllocationSize,
                                        size_t Alignment = g_LinearStrategyAlignment) noexcept;

    static void Free(SlabDescriptor& SlabToFree, void* MemoryAddressToFree) noexcept;
    static void Reset(SlabDescriptor& SlabToReset) noexcept;

    static void RewindToMarker(SlabDescriptor& SlabToRewind, uintptr_t RewindOffset) noexcept;
};

} // namespace Allocator
