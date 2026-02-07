#include <modules/strategies/linear_module/linear_module.h>

namespace Allocator {

template <typename TContext>
thread_local LinearModuleThreadGuard<TContext> LinearStrategyModule<TContext>::g_ThreadGuard;

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
    auto& tls = GetTLS();
    SlabDescriptor* NextSlab = tls.ActiveSlab->GetNextSlab();

    if (NextSlab != nullptr) {
        tls.ActiveSlab = NextSlab;
        LinearStrategy::Reset(*tls.ActiveSlab);
        return Allocate(AllocationSize, AllocationAlignment);
    }

    GrowSlabChain();
    return (tls.ActiveSlab != nullptr) ? Allocate(AllocationSize, AllocationAlignment) : nullptr;
}

template <typename TContext> void LinearStrategyModule<TContext>::GrowSlabChain() noexcept
{
    auto& tls = GetTLS();
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);

    if (!Registry) {
        LOG_ALLOCATOR("CRITICAL", "LinearModule: Growth failed - Registry is NULL.");
        return;
    }

    SlabDescriptor* NewSlab = Registry->AllocateSlab();
    if (!NewSlab)
        return;

    NewSlab->SetNextSlab(nullptr);

    if (!tls.HeadSlab) {
        tls.HeadSlab = NewSlab;
        tls.ActiveSlab = NewSlab;
        RegisterThreadContext(&tls.HeadSlab);
    }
    else {
        tls.ActiveSlab->SetNextSlab(NewSlab);
        tls.ActiveSlab = NewSlab;
    }
}

template <typename TContext>
void LinearStrategyModule<TContext>::InitializeModule(SlabRegistry* RegistryInstance) noexcept
{
    LOG_ALLOCATOR("INFO", "LinearModule: Initializing with Registry: " << RegistryInstance);
    g_SlabRegistry.store(RegistryInstance, std::memory_order_release);
}

template <typename TContext> void LinearStrategyModule<TContext>::ShutdownModule() noexcept
{
    auto& tls = GetTLS();
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);

    UnregisterThreadContext(&tls.HeadSlab);

    SlabDescriptor* Current = tls.HeadSlab;
    while (Current) {
        SlabDescriptor* Next = Current->GetNextSlab();
        if (Registry)
            Registry->FreeSlab(Current);
        Current = Next;
    }
    tls.HeadSlab = tls.ActiveSlab = nullptr;
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
    auto& tls = GetTLS();
    if (tls.HeadSlab != nullptr) {
        SlabDescriptor* Current = tls.HeadSlab;
        while (Current) {
            LinearStrategy::Reset(*Current);
            Current = Current->GetNextSlab();
        }
        tls.ActiveSlab = tls.HeadSlab;
        FlushThreadStats();
    }
}

template <typename TContext>
void LinearStrategyModule<TContext>::RegisterThreadContext(SlabDescriptor** ThreadHeadPtr) noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    g_ThreadHeads.push_back(ThreadHeadPtr);
}

template <typename TContext>
void LinearStrategyModule<TContext>::UnregisterThreadContext(
    SlabDescriptor** ThreadHeadPtr) noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    std::erase(g_ThreadHeads, ThreadHeadPtr);
}

template <typename TContext>
std::pair<SlabDescriptor*, uintptr_t> LinearStrategyModule<TContext>::GetCurrentState() noexcept
    requires(TContext::IsRewindable)
{
    auto& tls = GetTLS();
    return tls.ActiveSlab ? std::make_pair(tls.ActiveSlab, tls.ActiveSlab->GetFreeListHead())
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
    if (m_HasState && m_MarkedSlab)
        LinearStrategyModule<TContext>::RewindState(m_MarkedSlab, m_MarkedOffset);
}

template class LinearStrategyModule<FrameLoad>;
template class LinearStrategyModule<LevelLoad>;
template class LinearStrategyModule<GlobalLoad>;
template class LinearModuleThreadGuard<FrameLoad>;
template class LinearModuleThreadGuard<LevelLoad>;
template class LinearModuleThreadGuard<GlobalLoad>;
} // namespace Allocator
