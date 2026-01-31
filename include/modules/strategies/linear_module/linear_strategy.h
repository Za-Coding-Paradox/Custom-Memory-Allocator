#pragma once

#include <modules/strategies/linear_module/linear_scopes.h>

namespace Allocator {

// FIX (Bug #15): Increased from 8 to 16 for SSE/SIMD support
static constexpr size_t g_LinearStrategyAlignment = 16;
static constexpr uintptr_t g_LinearStrategyAlignmentMask =
    g_LinearStrategyAlignment - 1;

class LinearStrategy {
public:
  LinearStrategy() = delete;

  // Added Alignment parameter (defaults to 16)
  [[nodiscard]] static bool
  CanFit(const SlabDescriptor &AllocationSlab, size_t AllocationSize,
         size_t Alignment = g_LinearStrategyAlignment) noexcept;

  // Added Alignment parameter (defaults to 16)
  [[nodiscard]] static void *
  Allocate(SlabDescriptor &AllocationSlab, size_t AllocationSize,
           size_t Alignment = g_LinearStrategyAlignment) noexcept;

  static void Free(SlabDescriptor &SlabToFree,
                   void *MemoryAddressToFree) noexcept;
  static void Reset(SlabDescriptor &SlabToReset) noexcept;

  static void RewindToMarker(SlabDescriptor &SlabToRewind,
                             uintptr_t RewindOffset) noexcept;
};

} // namespace Allocator
