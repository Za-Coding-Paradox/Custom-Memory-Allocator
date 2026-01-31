#pragma once

#include <modules/allocator_registry.h>

namespace Allocator {

// FIX (Bug #2 and original namespace issue): Changed from namespace to struct
// Templates require TYPES as arguments, not namespaces

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
