#include <modules/strategies/linear_module/linear_module.h>

namespace Allocator {

// Static definitions
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
std::atomic<SlabRegistry*> LinearStrategyModule<TContext>::g_SlabRegistry{nullptr};
template <typename TContext> ContextStats LinearStrategyModule<TContext>::g_GlobalStats;
template <typename TContext> std::mutex LinearStrategyModule<TContext>::g_ContextMutex;
template <typename TContext>
std::vector<SlabDescriptor**> LinearStrategyModule<TContext>::g_ThreadHeads;

template <typename TContext>
void LinearStrategyModule<TContext>::InitializeModule(SlabRegistry* RegistryInstance) noexcept {
  LOG_ALLOCATOR("INFO", "LinearModule: Initialized.");
  g_SlabRegistry.store(RegistryInstance, std::memory_order_release);
}

template <typename TContext> void LinearStrategyModule<TContext>::ShutdownModule() noexcept {
  FlushThreadStats();
  UnregisterThreadContext();

  SlabRegistry* registry = g_SlabRegistry.load(std::memory_order_acquire);
  if (!registry) {
    g_HeadSlab = nullptr;
    g_ActiveSlab = nullptr;
    return;
  }

  SlabDescriptor* Current = g_HeadSlab;
  while (Current) {
    SlabDescriptor* Next = Current->GetNextSlab();
    registry->FreeSlab(Current);
    Current = Next;
  }

  g_HeadSlab = nullptr;
  g_ActiveSlab = nullptr;
}

template <typename TContext> void LinearStrategyModule<TContext>::ShutdownSystem() noexcept {
  LOG_ALLOCATOR("WARN", "LinearModule: System Shutdown Initiated.");

  SlabRegistry* registry = g_SlabRegistry.exchange(nullptr, std::memory_order_acq_rel);
  if (!registry)
    return;

  std::lock_guard<std::mutex> lock(g_ContextMutex);
  for (SlabDescriptor** threadHeadPtr : g_ThreadHeads) {
    if (!threadHeadPtr || !(*threadHeadPtr))
      continue;

    SlabDescriptor* current = *threadHeadPtr;
    while (current) {
      SlabDescriptor* next = current->GetNextSlab();
      registry->FreeSlab(current);
      current = next;
    }
    *threadHeadPtr = nullptr;
  }

  g_ThreadHeads.clear();
  LOG_ALLOCATOR("INFO", "LinearModule: System Shutdown Complete.");
}

template <typename TContext>
void* LinearStrategyModule<TContext>::OverFlowAllocate(size_t AllocationSize,
                                                       size_t AllocationAlignment) noexcept {
  SlabDescriptor* NextSlab = g_ActiveSlab->GetNextSlab();

  if (NextSlab) {
    g_ActiveSlab = NextSlab;

    LinearStrategy::RewindToMarker(*g_ActiveSlab, g_ActiveSlab->GetSlabStart());
    g_ActiveSlab->SetActiveSlots(0);

    if (LinearStrategy::CanFit(*g_ActiveSlab, AllocationSize)) {
      return LinearStrategy::Allocate(*g_ActiveSlab, AllocationSize, AllocationAlignment);
    }
    return OverFlowAllocate(AllocationSize, AllocationAlignment);
  }

  GrowSlabChain();
  if (!g_ActiveSlab || (g_ActiveSlab == NextSlab))
    return nullptr;

  return Allocate(AllocationSize, AllocationAlignment);
}

template <typename TContext>
void* LinearStrategyModule<TContext>::Allocate(size_t AllocationSize,
                                               size_t AllocationAlignment) noexcept {
  (void)g_ThreadGuard;

  if (AllocationSize > g_ConstSlabSize) [[unlikely]]
    return nullptr;
  if (!g_ActiveSlab) [[unlikely]]
    GrowSlabChain();
  if (!g_ActiveSlab) [[unlikely]]
    return nullptr;

  void* ptr = nullptr;
  if (LinearStrategy::CanFit(*g_ActiveSlab, AllocationSize)) {
    ptr = LinearStrategy::Allocate(*g_ActiveSlab, AllocationSize, AllocationAlignment);
  } else {
    ptr = OverFlowAllocate(AllocationSize, AllocationAlignment);
  }

  if (ptr) {
    g_ThreadAllocated += AllocationSize;
    g_ThreadCount++;
    if (g_ThreadAllocated > g_ThreadPeak)
      g_ThreadPeak = g_ThreadAllocated;
  }
  return ptr;
}

template <typename TContext> void LinearStrategyModule<TContext>::GrowSlabChain() noexcept {
  SlabRegistry* registry = g_SlabRegistry.load(std::memory_order_acquire);
  if (!registry) [[unlikely]]
    return;

  SlabDescriptor* NewSlab = registry->AllocateSlab();
  if (!NewSlab) [[unlikely]]
    return;

  NewSlab->SetNextSlab(nullptr);

  if (!g_HeadSlab) {
    g_HeadSlab = NewSlab;
    g_ActiveSlab = NewSlab;
    RegisterThreadContext();
  } else {
    g_ActiveSlab->SetNextSlab(NewSlab);
    g_ActiveSlab = NewSlab;
  }
}

template <typename TContext> void LinearStrategyModule<TContext>::RegisterThreadContext() noexcept {
  std::lock_guard<std::mutex> lock(g_ContextMutex);
  g_ThreadHeads.push_back(&g_HeadSlab);
}

template <typename TContext>
void LinearStrategyModule<TContext>::UnregisterThreadContext() noexcept {
  std::lock_guard<std::mutex> lock(g_ContextMutex);
  std::erase(g_ThreadHeads, &g_HeadSlab);
}

template <typename TContext>
void LinearStrategyModule<TContext>::Reset() noexcept
  requires(!TContext::IsRewindable)
{
  if (g_HeadSlab) {
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
  SlabRegistry* registry = g_SlabRegistry.load(std::memory_order_acquire);

  if (!SavedSlab) {
    SlabDescriptor* current = g_HeadSlab;
    while (current) {
      SlabDescriptor* next = current->GetNextSlab();
      if (registry)
        registry->FreeSlab(current);
      current = next;
    }
    g_HeadSlab = nullptr;
    g_ActiveSlab = nullptr;
    return;
  }

  SlabDescriptor* victim = SavedSlab->GetNextSlab();
  SavedSlab->SetNextSlab(nullptr);

  while (victim) {
    SlabDescriptor* nextVictim = victim->GetNextSlab();
    if (registry)
      registry->FreeSlab(victim);
    victim = nextVictim;
  }

  g_ActiveSlab = SavedSlab;
  LinearStrategy::RewindToMarker(*g_ActiveSlab, SavedOffset);
}

template <typename TContext>
std::pair<SlabDescriptor*, uintptr_t> LinearStrategyModule<TContext>::GetCurrentState() noexcept
  requires(TContext::IsRewindable)
{
  if (!g_ActiveSlab)
    return {nullptr, 0};
  return {g_ActiveSlab, g_ActiveSlab->GetFreeListHead()};
}

template <typename TContext>
LinearScopedMarker<TContext>::LinearScopedMarker() noexcept : m_HasState(true) {
  auto State = LinearStrategyModule<TContext>::GetCurrentState();
  m_MarkedSlab = State.first;
  m_MarkedOffset = State.second;
}

template <typename TContext> LinearScopedMarker<TContext>::~LinearScopedMarker() noexcept {
  if (m_HasState && m_MarkedSlab) {
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

    size_t curPeak = g_GlobalStats.PeakUsage.load(std::memory_order_relaxed);
    size_t newPeak = curPeak + g_ThreadPeak;
    while (newPeak > curPeak && !g_GlobalStats.PeakUsage.compare_exchange_weak(curPeak, newPeak)) {
    }

    g_ThreadAllocated = 0;
    g_ThreadCount = 0;
    g_ThreadPeak = 0;
  }
}

} // namespace Allocator

// Instantiations
namespace Allocator {
template class LinearStrategyModule<FrameLoad>;
template class LinearStrategyModule<LevelLoad>;
template class LinearStrategyModule<GlobalLoad>;

template class LinearModuleThreadGuard<FrameLoad>;
template class LinearModuleThreadGuard<LevelLoad>;
template class LinearModuleThreadGuard<GlobalLoad>;

template class LinearScopedMarker<LevelLoad>;
template class LinearScopedMarker<GlobalLoad>;
} // namespace Allocator
