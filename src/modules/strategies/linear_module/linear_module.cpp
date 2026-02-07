#include <algorithm>
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

template <typename TContext> thread_local size_t LinearStrategyModule<TContext>::g_ThreadFreed = 0;
template <typename TContext> thread_local size_t LinearStrategyModule<TContext>::g_ThreadPeak = 0;
template <typename TContext> thread_local size_t LinearStrategyModule<TContext>::g_ThreadCount = 0;

template <typename TContext>
std::atomic<SlabRegistry*> LinearStrategyModule<TContext>::g_SlabRegistry{nullptr};

template <typename TContext> ContextStats LinearStrategyModule<TContext>::g_GlobalStats;
template <typename TContext> std::mutex LinearStrategyModule<TContext>::g_ContextMutex;
template <typename TContext>
std::vector<SlabDescriptor**> LinearStrategyModule<TContext>::g_ThreadHeads;

template <typename TContext>
void* LinearStrategyModule<TContext>::OverFlowAllocate(size_t AllocationSize,
                                                       size_t AllocationAlignment) noexcept
{
    SlabDescriptor* NextSlab = g_ActiveSlab->GetNextSlab();

    if (NextSlab != nullptr) {
        g_ActiveSlab = NextSlab;
        LinearStrategy::Reset(*g_ActiveSlab);
        return Allocate(AllocationSize, AllocationAlignment);
    }

    GrowSlabChain();
    return (g_ActiveSlab != nullptr) ? Allocate(AllocationSize, AllocationAlignment) : nullptr;
}

template <typename TContext> void LinearStrategyModule<TContext>::GrowSlabChain() noexcept
{
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    if (!Registry) [[unlikely]]
        return;

    SlabDescriptor* NewSlab = Registry->AllocateSlab();
    if (!NewSlab) [[unlikely]]
        return;

    NewSlab->SetNextSlab(nullptr);

    if (!g_HeadSlab) {
        g_HeadSlab = NewSlab;
        g_ActiveSlab = NewSlab;
        RegisterThreadContext();
    }
    else {
        g_ActiveSlab->SetNextSlab(NewSlab);
        g_ActiveSlab = NewSlab;
    }
}

template <typename TContext>
void LinearStrategyModule<TContext>::InitializeModule(SlabRegistry* RegistryInstance) noexcept
{
    g_SlabRegistry.store(RegistryInstance, std::memory_order_release);
}

template <typename TContext> void LinearStrategyModule<TContext>::ShutdownModule() noexcept
{
    FlushThreadStats();
    UnregisterThreadContext();

    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    SlabDescriptor* Current = g_HeadSlab;

    while (Current) {
        SlabDescriptor* Next = Current->GetNextSlab();
        if (Registry)
            Registry->FreeSlab(Current);
        Current = Next;
    }
    g_HeadSlab = g_ActiveSlab = nullptr;
}

template <typename TContext> void LinearStrategyModule<TContext>::ShutdownSystem() noexcept
{
    SlabRegistry* Registry = g_SlabRegistry.exchange(nullptr, std::memory_order_acq_rel);
    if (!Registry)
        return;

    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    for (SlabDescriptor** HeadPtr : g_ThreadHeads) {
        if (HeadPtr && *HeadPtr) {
            SlabDescriptor* Current = *HeadPtr;
            while (Current) {
                SlabDescriptor* Next = Current->GetNextSlab();
                Registry->FreeSlab(Current);
                Current = Next;
            }
            *HeadPtr = nullptr;
        }
    }
    g_ThreadHeads.clear();
}

template <typename TContext>
void LinearStrategyModule<TContext>::Reset() noexcept
    requires(!TContext::IsRewindable)
{
    if (g_HeadSlab != nullptr) {
        ALLOCATOR_DIAGNOSTIC({ g_ThreadFreed += GetThreadTotalUsed(); });

        SlabDescriptor* Current = g_HeadSlab;
        while (Current) {
            LinearStrategy::Reset(*Current);
            Current = Current->GetNextSlab();
        }
        g_ActiveSlab = g_HeadSlab;
        FlushThreadStats();
    }
}

template <typename TContext>
void LinearStrategyModule<TContext>::RewindState(SlabDescriptor* SavedSlab,
                                                 uintptr_t SavedOffset) noexcept
    requires(TContext::IsRewindable)
{
    if (SavedSlab == nullptr) {
        ShutdownModule();
        return;
    }

    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    SlabDescriptor* Victim = SavedSlab->GetNextSlab();
    SavedSlab->SetNextSlab(nullptr);

    while (Victim != nullptr) {
        SlabDescriptor* NextVictim = Victim->GetNextSlab();
        if (Registry)
            Registry->FreeSlab(Victim);
        Victim = NextVictim;
    }

    g_ActiveSlab = SavedSlab;
    LinearStrategy::RewindToMarker(*g_ActiveSlab, SavedOffset);
    FlushThreadStats();
}

template <typename TContext> void LinearStrategyModule<TContext>::FlushThreadStats() noexcept
{
    if (g_ThreadAllocated > 0 || g_ThreadFreed > 0) {
        g_GlobalStats.BytesAllocated.fetch_add(g_ThreadAllocated, std::memory_order_relaxed);
        g_GlobalStats.BytesFreed.fetch_add(g_ThreadFreed, std::memory_order_relaxed);
        g_GlobalStats.AllocationCount.fetch_add(g_ThreadCount, std::memory_order_relaxed);

        g_ThreadAllocated = 0;
        g_ThreadFreed = 0;
        g_ThreadCount = 0;
        g_ThreadPeak = 0;
    }
}

template <typename TContext> size_t LinearStrategyModule<TContext>::GetThreadTotalUsed() noexcept
{
    size_t TotalUsed = 0;
    SlabDescriptor* Current = g_HeadSlab;
    while (Current != nullptr) {
        TotalUsed += static_cast<size_t>(Current->GetFreeListHead() - Current->GetSlabStart());
        if (Current == g_ActiveSlab)
            break;
        Current = Current->GetNextSlab();
    }
    return TotalUsed;
}

template <typename TContext> void LinearStrategyModule<TContext>::RegisterThreadContext() noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    g_ThreadHeads.push_back(&g_HeadSlab);
}

template <typename TContext> void LinearStrategyModule<TContext>::UnregisterThreadContext() noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    std::erase(g_ThreadHeads, &g_HeadSlab);
}

template <typename TContext>
std::pair<SlabDescriptor*, uintptr_t> LinearStrategyModule<TContext>::GetCurrentState() noexcept
    requires(TContext::IsRewindable)
{
    return g_ActiveSlab ? std::make_pair(g_ActiveSlab, g_ActiveSlab->GetFreeListHead())
                        : std::make_pair(nullptr, 0);
}

template <typename TContext>
LinearScopedMarker<TContext>::LinearScopedMarker() noexcept : m_HasState(true)
{
    auto State = LinearStrategyModule<TContext>::GetCurrentState();
    m_MarkedSlab = State.first;
    m_MarkedOffset = State.second;
}

template <typename TContext> LinearScopedMarker<TContext>::~LinearScopedMarker() noexcept
{
    if (m_HasState && m_MarkedSlab) {
        LinearStrategyModule<TContext>::RewindState(m_MarkedSlab, m_MarkedOffset);
    }
}

template class LinearStrategyModule<FrameLoad>;
template class LinearStrategyModule<LevelLoad>;
template class LinearStrategyModule<GlobalLoad>;

template class LinearModuleThreadGuard<FrameLoad>;
template class LinearModuleThreadGuard<LevelLoad>;
template class LinearModuleThreadGuard<GlobalLoad>;

template class LinearScopedMarker<LevelLoad>;
template class LinearScopedMarker<GlobalLoad>;

} // namespace Allocator
