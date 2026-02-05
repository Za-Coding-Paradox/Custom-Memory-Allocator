#pragma once
#include <Core/core.h>

namespace Allocator {

const size_t g_GenerationShiftKey = 32;
const size_t g_MaximumBitMask = 0xFFFFFFFF;

class Utility {
public:
  [[nodiscard]] static void* AlignForward(void* Address, size_t Alignment) noexcept;
  [[nodiscard]] static void* AlignBackward(void* Address, size_t Alignment) noexcept;

  [[nodiscard]] static size_t GetPadding(void* Address, size_t Alignment) noexcept;
  [[nodiscard]] static size_t GetPaddingWithHeader(void* Address, size_t Alignment,
                                                   size_t HeaderSize) noexcept;
  [[nodiscard]] static void* Add(void* Address, size_t Offset) noexcept;
  [[nodiscard]] static void* Subtract(void* Address, size_t Offset) noexcept;

  [[nodiscard]] static size_t PointerDifference(void* Address1, void* Address2) noexcept;
  [[nodiscard]] static size_t GetRelativeAddress(void* Address, void* ArenaStart) noexcept;

  [[nodiscard]] static size_t GetSlabIndex(void* Address, void* ArenaStart,
                                           size_t SlabSize) noexcept;
  [[nodiscard]] static void* GetSlabStart(size_t SlabIndex, void* ArenaStart,
                                          size_t SlabSize) noexcept;

  [[nodiscard]] static bool IsPointerInArena(void* Address, void* ArenaStart,
                                             size_t ArenaSize) noexcept;
  [[nodiscard]] static bool IsPowerOfTwo(size_t Value) noexcept;
  [[nodiscard]] static bool IsPowerOfTwo(void* Address) noexcept;
};

} // namespace Allocator
