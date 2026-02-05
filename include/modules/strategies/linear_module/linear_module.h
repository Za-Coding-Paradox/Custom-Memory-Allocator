#pragma once

#include <algorithm>
#include <modules/allocation_stats.h>
#include <modules/strategies/linear_module/linear_strategy.h>
namespace Allocator {

// Forward Declarations
template <typename TContext> class LinearModuleThreadGuard;

// =================================================================================================
// LINEAR STRATEGY MODULE
// =================================================================================================
template <typename TContext> class LinearStrategyModule {
private:
  // -----------------------------------------------------------------------------------------------
  // THREAD LOCAL STATE (Fast Path)
  // -----------------------------------------------------------------------------------------------
  static thread_local SlabDescriptor* g_HeadSlab;
  static thread_local SlabDescriptor* g_ActiveSlab;

  static thread_local LinearModuleThreadGuard<TContext> g_ThreadGuard;

  static thread_local size_t g_ThreadAllocated;
  static thread_local size_t g_ThreadPeak;
  static thread_local size_t g_ThreadCount;

  // -----------------------------------------------------------------------------------------------
  // GLOBAL STATE (Shared)
  // -----------------------------------------------------------------------------------------------
  // Definitions moved to .cpp to satisfy Linker
  static std::atomic<SlabRegistry*> g_SlabRegistry;
  static ContextStats g_GlobalStats;

  // -----------------------------------------------------------------------------------------------
  // THREAD CONTEXT REGISTRY
  // -----------------------------------------------------------------------------------------------
  static std::mutex g_ContextMutex;
  static std::vector<SlabDescriptor**> g_ThreadHeads;

  // -----------------------------------------------------------------------------------------------
  // INTERNAL HELPERS
  // -----------------------------------------------------------------------------------------------
  static void* OverFlowAllocate(size_t AllocationSize, size_t AllocationAlignment) noexcept;
  static void GrowSlabChain() noexcept;

  static void RegisterThreadContext() noexcept;
  static void UnregisterThreadContext() noexcept;

  friend class LinearModuleThreadGuard<TContext>;
  friend class AllocatorEngine;

public:
  LinearStrategyModule() = delete;

  static void InitializeModule(SlabRegistry* RegistryInstance) noexcept;
  static void ShutdownModule() noexcept;
  static void ShutdownSystem() noexcept;

  [[nodiscard]] static void* Allocate(size_t AllocationSize, size_t AllocationAlignment) noexcept;

  static void FlushThreadStats() noexcept;
  static ContextStats& GetGlobalStats() noexcept { return g_GlobalStats; }

  static void Free(SlabDescriptor& SlabToFree, void* MemoryAddressToFree) noexcept = delete;

  static void Reset() noexcept
    requires(!TContext::IsRewindable);

  static void RewindState(SlabDescriptor* SavedSlab, uintptr_t SavedOffset) noexcept
    requires(TContext::IsRewindable);

  [[nodiscard]] static std::pair<SlabDescriptor*, uintptr_t> GetCurrentState() noexcept
    requires(TContext::IsRewindable);
};

// =================================================================================================
// HELPER CLASSES
// =================================================================================================

template <typename TContext> class LinearModuleThreadGuard {
public:
  LinearModuleThreadGuard() = default;

  ~LinearModuleThreadGuard() noexcept {
    LinearStrategyModule<TContext>::FlushThreadStats();
    if (LinearStrategyModule<TContext>::g_HeadSlab != nullptr) {
      LinearStrategyModule<TContext>::ShutdownModule();
    }
  }
};

template <typename TContext> class LinearScopedMarker {
  static_assert(TContext::IsRewindable,
                "[DEBUG] LinearScopedMarker: Context does not support rewinding.");

  SlabDescriptor* m_MarkedSlab;
  uintptr_t m_MarkedOffset;
  bool m_HasState;

public:
  LinearScopedMarker() noexcept;
  ~LinearScopedMarker() noexcept;
  void Commit() noexcept;
};

} // namespace Allocator
