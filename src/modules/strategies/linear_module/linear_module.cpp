#include <modules/allocator_engine.h> // Required for AllocationStats definition
#include <modules/strategies/linear_module/linear_module.h>

namespace Allocator {

template <typename TContext>
thread_local SlabDescriptor* LinearStrategyModule<TContext>::g_HeadSlab = nullptr;

template <typename TContext>
thread_local SlabDescriptor* LinearStrategyModule<TContext>::g_ActiveSlab = nullptr;

template <typename TContext>
thread_local LinearModuleThreadGuard<TContext> LinearStrategyModule<TContext>::g_ThreadGuard;

template <typename TContext>
thread_local size_t LinearStrategyModule<TContext>::g_ThreadAllocated = 0;

template <typename TContext> thread_local size_t LinearStrategyModule<TContext>::g_ThreadPeak = 0;

template <typename TContext> thread_local size_t LinearStrategyModule<TContext>::g_ThreadCount = 0;

template <typename TContext>
void LinearStrategyModule<TContext>::InitializeModule(SlabRegistry* RegistryInstance) noexcept {
  LOG_ALLOCATOR("INFO", "LinearModule: Initialized.");
  g_SlabRegistry.store(RegistryInstance, std::memory_order_release);
}

template <typename TContext> void LinearStrategyModule<TContext>::ShutdownModule() noexcept {
  FlushThreadStats();

  LOG_ALLOCATOR("INFO", "LinearModule: Shutting down...");

  SlabRegistry* registry = g_SlabRegistry.load(std::memory_order_acquire);

  if (!registry) {
    g_HeadSlab = nullptr;
    g_ActiveSlab = nullptr;
    return;
  }

  size_t FreedCount = 0;

  SlabDescriptor* Current = g_HeadSlab;
  while (static_cast<bool>(Current)) {
    SlabDescriptor* Next = Current->GetNextSlab();
    registry->FreeSlab(Current);
    Current = Next;
    FreedCount++;
  }

  g_HeadSlab = nullptr;
  g_ActiveSlab = nullptr;

  LOG_ALLOCATOR("INFO", "LinearModule: Shutdown Complete. Freed Slabs: " << FreedCount);
}

template <typename TContext>
void* LinearStrategyModule<TContext>::Allocate(size_t AllocationSize,
                                               size_t AllocationAlignment) noexcept {

  if (AllocationSize > g_ConstSlabSize) [[unlikely]] {
    LOG_ALLOCATOR("ERROR", "LinearModule: Allocation size "
                               << AllocationSize << " exceeds Slab Size " << g_ConstSlabSize);
    return nullptr;
  }

  if (!static_cast<bool>(g_ActiveSlab)) [[unlikely]] {
    GrowSlabChain();
  }

  if (!static_cast<bool>(g_ActiveSlab)) [[unlikely]] {
    return nullptr;
  }

  void* ptr = nullptr;

  if (LinearStrategy::CanFit(*g_ActiveSlab, AllocationSize)) {
    ptr = LinearStrategy::Allocate(*g_ActiveSlab, AllocationSize, AllocationAlignment);
  } else {
    ptr = OverFlowAllocate(AllocationSize, AllocationAlignment);
  }

  if (ptr != nullptr) {
    g_ThreadAllocated += AllocationSize;
    g_ThreadCount++;

    if (g_ThreadAllocated > g_ThreadPeak) {
      g_ThreadPeak = g_ThreadAllocated;
    }
  }

  return ptr;
}

template <typename TContext>
void* LinearStrategyModule<TContext>::OverFlowAllocate(size_t AllocationSize,
                                                       size_t AllocationAlignment) noexcept {

  SlabDescriptor* NextSlab = g_ActiveSlab->GetNextSlab();

  if (static_cast<bool>(NextSlab)) {
    LOG_ALLOCATOR("DEBUG", "LinearModule: Using Zombie Slab (Reuse).");
    g_ActiveSlab = NextSlab;

    LinearStrategy::RewindToMarker(*g_ActiveSlab, g_ActiveSlab->GetSlabStart());
    g_ActiveSlab->SetActiveSlots(0);

    if (LinearStrategy::CanFit(*g_ActiveSlab, AllocationSize)) {
      return LinearStrategy::Allocate(*g_ActiveSlab, AllocationSize);
    }
    return OverFlowAllocate(AllocationSize, AllocationAlignment);
  }

  GrowSlabChain();
  if (!g_ActiveSlab || g_ActiveSlab->GetNextSlab() == nullptr && g_ActiveSlab == NextSlab) {
    return nullptr;
  }
  return Allocate(AllocationSize, AllocationAlignment);
}

template <typename TContext> void LinearStrategyModule<TContext>::GrowSlabChain() noexcept {
  LOG_ALLOCATOR("DEBUG", "LinearModule: Growing Chain (New Slab).");

  // FIX: Atomic Load
  SlabRegistry* registry = g_SlabRegistry.load(std::memory_order_acquire);

  if (!registry) [[unlikely]] {
    LOG_ALLOCATOR("CRITICAL", "LinearModule: Attempted to grow chain after Registry shutdown.");
    return;
  }

  SlabDescriptor* NewSlab = registry->AllocateSlab();

  if (!NewSlab) [[unlikely]] {
    LOG_ALLOCATOR("CRITICAL", "LinearModule: Registry OOM - Failed to allocate slab.");
    return;
  }

  NewSlab->SetNextSlab(nullptr);

  if (!static_cast<bool>(g_HeadSlab)) {
    g_HeadSlab = NewSlab;
    g_ActiveSlab = NewSlab;
  } else {
    g_ActiveSlab->SetNextSlab(NewSlab);
    g_ActiveSlab = NewSlab;
  }
}

template <typename TContext>
void LinearStrategyModule<TContext>::Reset() noexcept
  requires(!TContext::IsRewindable)
{
  if (static_cast<bool>(g_HeadSlab)) {
    LOG_ALLOCATOR("DEBUG", "LinearModule: Frame Reset (Lazy).");
    SlabDescriptor* NextSlab = g_HeadSlab->GetNextSlab();
    LinearStrategy::Reset(*g_HeadSlab);
    g_HeadSlab->SetNextSlab(NextSlab);
    g_ActiveSlab = g_HeadSlab;
    FlushThreadStats();
  }
}

template <typename TContext>
void LinearStrategyModule<TContext>::RewindState(SlabDescriptor* SavedSlab,
                                                 uintptr_t SavedOffset) noexcept
  requires(TContext::IsRewindable)
{
  LOG_ALLOCATOR("DEBUG", "LinearModule: Rewinding State.");

  SlabRegistry* registry = g_SlabRegistry.load(std::memory_order_acquire);

  if (!SavedSlab) {

    SlabDescriptor* current = g_HeadSlab;
    while (current != nullptr) {
      SlabDescriptor* next = current->GetNextSlab();

      if (registry) {
        registry->FreeSlab(current);
      }
      current = next;
    }

    g_HeadSlab = nullptr;
    g_ActiveSlab = nullptr;
    return;
  }

  SlabDescriptor* victim = SavedSlab->GetNextSlab();
  SavedSlab->SetNextSlab(nullptr);

  while (victim != nullptr) {
    SlabDescriptor* nextVictim = victim->GetNextSlab();

    if (registry) {
      registry->FreeSlab(victim);
    }

    victim = nextVictim;
  }

  g_ActiveSlab = SavedSlab;
  LinearStrategy::RewindToMarker(*g_ActiveSlab, SavedOffset);
}

template <typename TContext>
std::pair<SlabDescriptor*, uintptr_t> LinearStrategyModule<TContext>::GetCurrentState() noexcept
  requires(TContext::IsRewindable)
{
  if (!static_cast<bool>(g_ActiveSlab)) {
    return {nullptr, 0};
  }
  return {g_ActiveSlab, g_ActiveSlab->GetFreeListHead()};
}

template <typename TContext>
LinearScopedMarker<TContext>::LinearScopedMarker() noexcept : m_HasState(true) {
  auto State = LinearStrategyModule<TContext>::GetCurrentState();
  m_MarkedSlab = State.first;
  m_MarkedOffset = State.second;
}

template <typename TContext> LinearScopedMarker<TContext>::~LinearScopedMarker() noexcept {
  if (m_HasState && static_cast<bool>(m_MarkedSlab)) {
    LinearStrategyModule<TContext>::RewindState(m_MarkedSlab, m_MarkedOffset);
  }
}

template <typename TContext> void LinearScopedMarker<TContext>::Commit() noexcept {
  m_HasState = false;
}

template <typename TContext> void LinearStrategyModule<TContext>::FlushThreadStats() noexcept {
  if (g_ThreadAllocated > 0) {
    g_GlobalStats.BytesAllocated.fetch_add(g_ThreadAllocated, std::memory_order_relaxed);
    g_GlobalStats.AllocationCount.fetch_add(g_ThreadCount, std::memory_order_relaxed);

    size_t currentGlobalPeak = g_GlobalStats.PeakUsage.load(std::memory_order_relaxed);
    size_t newPeak = currentGlobalPeak + g_ThreadPeak;
    while (newPeak > currentGlobalPeak &&
           !g_GlobalStats.PeakUsage.compare_exchange_weak(currentGlobalPeak, newPeak)) {
      // Retry
    }

    g_ThreadAllocated = 0;
    g_ThreadCount = 0;
    g_ThreadPeak = 0;
  }
}

} // namespace Allocator

namespace Allocator {
// Instantiate LinearStrategyModule for all three context types
template class LinearStrategyModule<FrameLoad>;
template class LinearStrategyModule<LevelLoad>;
template class LinearStrategyModule<GlobalLoad>;

// Instantiate LinearModuleThreadGuard for cleanup
template class LinearModuleThreadGuard<FrameLoad>;
template class LinearModuleThreadGuard<LevelLoad>;
template class LinearModuleThreadGuard<GlobalLoad>;

// Instantiate LinearScopedMarker only for rewindable contexts
template class LinearScopedMarker<LevelLoad>;
template class LinearScopedMarker<GlobalLoad>;
} // namespace Allocator
