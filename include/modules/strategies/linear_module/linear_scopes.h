#pragma once

#include <modules/allocator_registry.h>

namespace Allocator {

struct FrameLoad {
  static constexpr bool IsRewindable = false;
};

struct LevelLoad {
  static constexpr bool IsRewindable = true;
};

struct GlobalLoad {
  static constexpr bool IsRewindable = true;
};

} // namespace Allocator
