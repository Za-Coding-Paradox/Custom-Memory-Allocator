#pragma once

#include <modules/allocator_registry.h>

namespace Allocator {

struct FrameLoad {
  static constexpr bool IsRewindable = false;
  static constexpr bool SupportsHandles = true;
};

struct LevelLoad {
  static constexpr bool IsRewindable = true;
  static constexpr bool SupportsHandles = true;
};

struct GlobalLoad {
  static constexpr bool IsRewindable = true;
  static constexpr bool SupportsHandles = true;
};

} // namespace Allocator
