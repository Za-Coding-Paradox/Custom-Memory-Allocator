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

  SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
  if (!static_cast<bool>(Registry)) {
    g_HeadSlab = nullptr;
    g_ActiveSlab = nullptr;
    return;
  }

  SlabDescriptor* Current = g_HeadSlab;
  while (static_cast<bool>(Current)) {
    SlabDescriptor* Next = Current->GetNextSlab();
    Registry->FreeSlab(Current);
    Current = Next;
  }

  g_HeadSlab = nullptr;
  g_ActiveSlab = nullptr;
}

template <typename TContext> void LinearStrategyModule<TContext>::ShutdownSystem() noexcept {
  LOG_ALLOCATOR("WARN", "LinearModule: System Shutdown Initiated.");

  SlabRegistry* Registry = g_SlabRegistry.exchange(nullptr, std::memory_order_acq_rel);
  if (!static_cast<bool>(Registry)) {
    return;
  }

  std::lock_guard<std::mutex> Lock(g_ContextMutex);
  for (SlabDescriptor** ThreadHeadPtr : g_ThreadHeads) {
    if (!static_cast<bool>(ThreadHeadPtr) || !(*ThreadHeadPtr)) {
      continue;
    }

    SlabDescriptor* Current = *ThreadHeadPtr;
    while (static_cast<bool>(Current)) {
      SlabDescriptor* Next = Current->GetNextSlab();
      Registry->FreeSlab(Current);
      Current = Next;
    }
    *ThreadHeadPtr = nullptr;
  }

  g_ThreadHeads.clear();
  LOG_ALLOCATOR("INFO", "LinearModule: System Shutdown Complete.");
}

template <typename TContext>
void* LinearStrategyModule<TContext>::OverFlowAllocate(size_t AllocationSize,
                                                       size_t AllocationAlignment) noexcept {
  SlabDescriptor* NextSlab = g_ActiveSlab->GetNextSlab();

  if (static_cast<bool>(NextSlab)) {
    g_ActiveSlab = NextSlab;

    LinearStrategy::RewindToMarker(*g_ActiveSlab, g_ActiveSlab->GetSlabStart());
    g_ActiveSlab->SetActiveSlots(0);

    if (LinearStrategy::CanFit(*g_ActiveSlab, AllocationSize)) {
      return LinearStrategy::Allocate(*g_ActiveSlab, AllocationSize, AllocationAlignment);
    }
    return OverFlowAllocate(AllocationSize, AllocationAlignment);
  }

  GrowSlabChain();
  if (!static_cast<bool>(g_ActiveSlab) || (g_ActiveSlab == NextSlab)) {
    return nullptr;
  }

  return Allocate(AllocationSize, AllocationAlignment);
}

template <typename TContext>
void* LinearStrategyModule<TContext>::Allocate(size_t AllocationSize,
                                               size_t AllocationAlignment) noexcept {
  (void)g_ThreadGuard;

  if (AllocationSize > g_ConstSlabSize) [[unlikely]] {
    return nullptr;
  }

  if (!static_cast<bool>(g_ActiveSlab)) [[unlikely]] {
    GrowSlabChain();
  }

  if (!static_cast<bool>(g_ActiveSlab)) [[unlikely]] {
    return nullptr;
  }

  void* Ptr = nullptr;
  if (LinearStrategy::CanFit(*g_ActiveSlab, AllocationSize)) {
    Ptr = LinearStrategy::Allocate(*g_ActiveSlab, AllocationSize, AllocationAlignment);
  } else {
    Ptr = OverFlowAllocate(AllocationSize, AllocationAlignment);
  }

  if (static_cast<bool>(Ptr)) {
    g_ThreadAllocated += AllocationSize;
    g_ThreadCount++;
    if (g_ThreadAllocated > g_ThreadPeak) {
      g_ThreadPeak = g_ThreadAllocated;
    }
  }

  return Ptr;
}

template <typename TContext> void LinearStrategyModule<TContext>::GrowSlabChain() noexcept {
  SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
  if (!static_cast<bool>(Registry)) [[unlikely]] {
    return;
  }

  SlabDescriptor* NewSlab = Registry->AllocateSlab();
  if (!static_cast<bool>(NewSlab)) [[unlikely]] {
    return;
  }

  NewSlab->SetNextSlab(nullptr);

  if (!static_cast<bool>(g_HeadSlab)) {
    g_HeadSlab = NewSlab;
    g_ActiveSlab = NewSlab;
    RegisterThreadContext();
  } else {
    g_ActiveSlab->SetNextSlab(NewSlab);
    g_ActiveSlab = NewSlab;
  }
}

template <typename TContext> void LinearStrategyModule<TContext>::RegisterThreadContext() noexcept {
  std::lock_guard<std::mutex> Lock(g_ContextMutex);
  g_ThreadHeads.push_back(&g_HeadSlab);
}

template <typename TContext>
void LinearStrategyModule<TContext>::UnregisterThreadContext() noexcept {
  std::lock_guard<std::mutex> Lock(g_ContextMutex);
  std::erase(g_ThreadHeads, &g_HeadSlab);
}

template <typename TContext>
void LinearStrategyModule<TContext>::Reset() noexcept
  requires(!TContext::IsRewindable)
{
  if (static_cast<bool>(g_HeadSlab)) {
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
  SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);

  if (!static_cast<bool>(SavedSlab)) {
    SlabDescriptor* Current = g_HeadSlab;
    while (static_cast<bool>(Current)) {
      SlabDescriptor* Next = Current->GetNextSlab();
      if (static_cast<bool>(Registry)) {
        Registry->FreeSlab(Current);
      }
      Current = Next;
    }
    g_HeadSlab = nullptr;
    g_ActiveSlab = nullptr;
    return;
  }

  SlabDescriptor* Victim = SavedSlab->GetNextSlab();
  SavedSlab->SetNextSlab(nullptr);

  while (static_cast<bool>(Victim)) {
    SlabDescriptor* NextVictim = Victim->GetNextSlab();
    if (static_cast<bool>(Registry)) {
      Registry->FreeSlab(Victim);
    }
    Victim = NextVictim;
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

    size_t CurPeak = g_GlobalStats.PeakUsage.load(std::memory_order_relaxed);
    size_t NewPeak = CurPeak + g_ThreadPeak;
    while (NewPeak > CurPeak && !g_GlobalStats.PeakUsage.compare_exchange_weak(CurPeak, NewPeak)) {
    }

    g_ThreadAllocated = 0;
    g_ThreadCount = 0;
    g_ThreadPeak = 0;
  }
}

} // namespace Allocator

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
