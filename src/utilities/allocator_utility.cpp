#include <utilities/allocator_utility.h>

namespace Allocator {

[[nodiscard]] auto Utility::AlignForward(void* Address, size_t Alignment) noexcept -> void* {
  assert(IsPowerOfTwo(Alignment) && "AlignForward: Alignment must be power of 2");

  const auto AddressVal = std::bit_cast<uintptr_t>(Address);
  const auto AlignmentMask = static_cast<uintptr_t>(Alignment - 1);
  const auto AlignedAddress = (AddressVal + AlignmentMask) & ~AlignmentMask;

  return std::bit_cast<void*>(AlignedAddress);
}

[[nodiscard]] auto Utility::AlignBackward(void* Address, size_t Alignment) noexcept -> void* {
  assert(IsPowerOfTwo(Alignment) && "AlignBackward: Alignment must be power of 2");

  const auto AddressVal = std::bit_cast<uintptr_t>(Address);
  const auto AlignmentMask = static_cast<uintptr_t>(Alignment - 1);
  const auto AlignedAddress = AddressVal & ~AlignmentMask;

  return std::bit_cast<void*>(AlignedAddress);
}

[[nodiscard]] auto Utility::Add(void* Address, size_t Offset) noexcept -> void* {
  const auto AddressVal = std::bit_cast<uintptr_t>(Address);
  const auto OffsetVal = static_cast<uintptr_t>(Offset);

  assert(AddressVal <= (UINTPTR_MAX - OffsetVal) && "Utility::Add: Pointer arithmetic overflow");

  return std::bit_cast<void*>(AddressVal + OffsetVal);
}

[[nodiscard]] auto Utility::Subtract(void* Address, size_t Offset) noexcept -> void* {
  const auto AddressVal = std::bit_cast<uintptr_t>(Address);
  const auto OffsetVal = static_cast<uintptr_t>(Offset);

  assert(AddressVal >= OffsetVal && "Utility::Subtract: Pointer arithmetic underflow");

  return std::bit_cast<void*>(AddressVal - OffsetVal);
}

[[nodiscard]] auto Utility::GetPadding(void* Address, size_t Alignment) noexcept -> size_t {
  const auto AddressVal = std::bit_cast<uintptr_t>(Address);
  const auto AlignmentMask = static_cast<uintptr_t>(Alignment - 1);
  const auto Modulo = AddressVal & AlignmentMask;

  return (Modulo == 0) ? 0 : (Alignment - static_cast<size_t>(Modulo));
}

[[nodiscard]] auto Utility::GetPaddingWithHeader(void* Address, size_t Alignment,
                                                 size_t HeaderSize) noexcept -> size_t {
  const auto AddressVal = std::bit_cast<uintptr_t>(Address);
  const auto DataStart = AddressVal + static_cast<uintptr_t>(HeaderSize);
  const auto AlignmentMask = static_cast<uintptr_t>(Alignment - 1);
  const auto Modulo = DataStart & AlignmentMask;

  return (Modulo == 0) ? 0 : (Alignment - static_cast<size_t>(Modulo));
}

[[nodiscard]] auto Utility::PointerDifference(void* Address1, void* Address2) noexcept -> size_t {
  return static_cast<size_t>(std::bit_cast<uintptr_t>(Address1) -
                             std::bit_cast<uintptr_t>(Address2));
}

[[nodiscard]] auto Utility::GetRelativeAddress(void* Address, void* ArenaStart) noexcept -> size_t {
  return static_cast<size_t>(std::bit_cast<uintptr_t>(Address) -
                             std::bit_cast<uintptr_t>(ArenaStart));
}

[[nodiscard]] auto Utility::GetSlabIndex(void* Address, void* ArenaStart, size_t SlabSize) noexcept
    -> size_t {
  const auto AddressVal = std::bit_cast<uintptr_t>(Address);
  const auto StartVal = std::bit_cast<uintptr_t>(ArenaStart);

  assert(AddressVal >= StartVal && "GetSlabIndex: Address is before ArenaStart");

  const auto RelativeOffset = AddressVal - StartVal;
  return static_cast<size_t>(RelativeOffset / SlabSize);
}

[[nodiscard]] auto Utility::GetSlabStart(size_t SlabIndex, void* ArenaStart,
                                         size_t SlabSize) noexcept -> void* {
  const auto ArenaStartVal = std::bit_cast<uintptr_t>(ArenaStart);
  const auto RelativeOffset = static_cast<uintptr_t>(SlabIndex) * static_cast<uintptr_t>(SlabSize);

  return std::bit_cast<void*>(ArenaStartVal + RelativeOffset);
}

[[nodiscard]] auto Utility::IsPointerInArena(void* Address, void* ArenaStart,
                                             size_t ArenaSize) noexcept -> bool {
  const auto AddressVal = std::bit_cast<uintptr_t>(Address);
  const auto StartVal = std::bit_cast<uintptr_t>(ArenaStart);

  return static_cast<size_t>(AddressVal - StartVal) < ArenaSize;
}

[[nodiscard]] auto Utility::IsPowerOfTwo(size_t Value) noexcept -> bool {
  return Value != 0 && (Value & (Value - 1)) == 0;
}

[[nodiscard]] auto Utility::IsPowerOfTwo(void* Address) noexcept -> bool {
  const auto AddressVal = std::bit_cast<uintptr_t>(Address);
  return AddressVal != 0 && (AddressVal & (AddressVal - 1)) == 0;
}

// --- Handle Packing Implementation ---

[[nodiscard]] auto Utility::PackHandle(size_t Index, size_t Generation) noexcept -> size_t {
  return (Generation << g_GenerationShiftKey) | (Index & g_MaximumBitMask);
}

[[nodiscard]] auto Utility::UnpackHandleIndex(size_t HandleID) noexcept -> size_t {
  return HandleID & g_MaximumBitMask;
}

[[nodiscard]] auto Utility::UnpackHandleGeneration(size_t HandleID) noexcept -> size_t {
  return HandleID >> g_GenerationShiftKey;
}

} // namespace Allocator
