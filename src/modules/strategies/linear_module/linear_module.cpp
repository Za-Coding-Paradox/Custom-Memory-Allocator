#include <algorithm>
#include <bit>
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

template <typename TContext> thread_local size_t LinearStrategyModule<TContext>::g_ThreadCount = 0;

template <typename TContext> thread_local size_t LinearStrategyModule<TContext>::g_ThreadPeak = 0;

template <typename TContext>
std::atomic<SlabRegistry*> LinearStrategyModule<TContext>::g_SlabRegistry{nullptr};

template <typename TContext> ContextStats LinearStrategyModule<TContext>::g_GlobalStats;

template <typename TContext> std::mutex LinearStrategyModule<TContext>::g_ContextMutex;

template <typename TContext>
std::vector<SlabDescriptor**> LinearStrategyModule<TContext>::g_ThreadHeads;

template <typename TContext>
void LinearStrategyModule<TContext>::InitializeModule(SlabRegistry* RegistryInstance) noexcept
{
    if (RegistryInstance != nullptr) {
        g_SlabRegistry.store(RegistryInstance, std::memory_order_release);
    }
}

template <typename TContext>
[[nodiscard]] void* LinearStrategyModule<TContext>::Allocate(size_t AllocationSize,
                                                             size_t AllocationAlignment) noexcept
{
    (void)g_ThreadGuard;

    if (AllocationSize > g_ConstSlabSize) [[unlikely]] {
        return nullptr;
    }

    if (g_ActiveSlab == nullptr) [[unlikely]] {
        GrowSlabChain();
    }

    if (g_ActiveSlab == nullptr) [[unlikely]] {
        return nullptr;
    }

    void* Result = nullptr;

    if (LinearStrategy::CanFit(*g_ActiveSlab, AllocationSize, AllocationAlignment)) {
        Result = LinearStrategy::Allocate(*g_ActiveSlab, AllocationSize, AllocationAlignment);
    }
    else {
        Result = OverFlowAllocate(AllocationSize, AllocationAlignment);
    }

    if (Result != nullptr) [[likely]] {
        g_ThreadAllocated += AllocationSize;
        g_ThreadCount++;

        size_t CurrentUsed = GetThreadTotalUsed();
        if (CurrentUsed > g_ThreadPeak) {
            g_ThreadPeak = CurrentUsed;
        }
    }

    return Result;
}

template <typename TContext>
void* LinearStrategyModule<TContext>::OverFlowAllocate(size_t AllocationSize,
                                                       size_t AllocationAlignment) noexcept
{
    SlabDescriptor* NextSlab = g_ActiveSlab->GetNextSlab();

    if (NextSlab != nullptr) {
        g_ActiveSlab = NextSlab;

        LinearStrategy::RewindToMarker(*g_ActiveSlab, g_ActiveSlab->GetSlabStart());
        g_ActiveSlab->SetActiveSlots(0);

        if (LinearStrategy::CanFit(*g_ActiveSlab, AllocationSize, AllocationAlignment)) {
            return LinearStrategy::Allocate(*g_ActiveSlab, AllocationSize, AllocationAlignment);
        }
        return OverFlowAllocate(AllocationSize, AllocationAlignment);
    }

    GrowSlabChain();
    if (g_ActiveSlab == nullptr) {
        return nullptr;
    }

    return Allocate(AllocationSize, AllocationAlignment);
}

template <typename TContext> size_t LinearStrategyModule<TContext>::GetThreadTotalUsed() noexcept
{
    size_t TotalUsed = 0;
    SlabDescriptor* Current = g_HeadSlab;
    while (Current != nullptr) {
        TotalUsed += static_cast<size_t>(Current->GetFreeListHead() - Current->GetSlabStart());
        if (Current == g_ActiveSlab) {
            break;
        }
        Current = Current->GetNextSlab();
    }
    return TotalUsed;
}

template <typename TContext> void LinearStrategyModule<TContext>::FlushThreadStats() noexcept
{
    if (g_ThreadAllocated > 0 || g_ThreadFreed > 0) {
        g_GlobalStats.BytesAllocated.fetch_add(g_ThreadAllocated, std::memory_order_relaxed);
        g_GlobalStats.BytesFreed.fetch_add(g_ThreadFreed, std::memory_order_relaxed);
        g_GlobalStats.AllocationCount.fetch_add(g_ThreadCount, std::memory_order_relaxed);

        const size_t TotalAlloc = g_GlobalStats.BytesAllocated.load(std::memory_order_relaxed);
        const size_t TotalFreed = g_GlobalStats.BytesFreed.load(std::memory_order_relaxed);
        const size_t CurrentGlobalUsage = (TotalAlloc > TotalFreed) ? (TotalAlloc - TotalFreed) : 0;

        size_t GlobalPeak = g_GlobalStats.PeakUsage.load(std::memory_order_relaxed);
        while (CurrentGlobalUsage > GlobalPeak) {
            if (g_GlobalStats.PeakUsage.compare_exchange_weak(GlobalPeak, CurrentGlobalUsage,
                                                              std::memory_order_relaxed)) {
                break;
            }
        }

        g_ThreadAllocated = 0;
        g_ThreadFreed = 0;
        g_ThreadCount = 0;
        g_ThreadPeak = 0;
    }
}

template <typename TContext>
void LinearStrategyModule<TContext>::Reset() noexcept
    requires(!TContext::IsRewindable)
{
    if (g_HeadSlab != nullptr) {
        g_ThreadFreed += GetThreadTotalUsed();

        SlabDescriptor* Next = g_HeadSlab->GetNextSlab();
        LinearStrategy::Reset(*g_HeadSlab);
        g_HeadSlab->SetNextSlab(Next);
        g_ActiveSlab = g_HeadSlab;

        FlushThreadStats();
    }
}

template <typename TContext>
void LinearStrategyModule<TContext>::RewindState(SlabDescriptor* SavedSlab,
                                                 uintptr_t SavedOffset) noexcept
    requires(TContext::IsRewindable)
{
    const size_t UsedBefore = GetThreadTotalUsed();
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);

    if (SavedSlab == nullptr) {
        ShutdownModule();
        g_ThreadFreed += UsedBefore;
        return;
    }

    SlabDescriptor* Victim = SavedSlab->GetNextSlab();
    SavedSlab->SetNextSlab(nullptr);

    while (Victim != nullptr) {
        SlabDescriptor* NextVictim = Victim->GetNextSlab();
        if (Registry != nullptr) {
            Registry->FreeSlab(Victim);
        }
        Victim = NextVictim;
    }

    g_ActiveSlab = SavedSlab;
    LinearStrategy::RewindToMarker(*g_ActiveSlab, SavedOffset);

    const size_t UsedAfter = GetThreadTotalUsed();
    g_ThreadFreed += (UsedBefore - UsedAfter);
}

template <typename TContext>
[[nodiscard]] std::pair<SlabDescriptor*, uintptr_t>
LinearStrategyModule<TContext>::GetCurrentState() noexcept
    requires(TContext::IsRewindable)
{
    if (g_ActiveSlab == nullptr) {
        return {nullptr, 0};
    }
    return {g_ActiveSlab, g_ActiveSlab->GetFreeListHead()};
}

template <typename TContext> void LinearStrategyModule<TContext>::GrowSlabChain() noexcept
{
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    if (Registry == nullptr) [[unlikely]] {
        return;
    }

    SlabDescriptor* NewSlab = Registry->AllocateSlab();
    if (NewSlab == nullptr) [[unlikely]] {
        return;
    }

    NewSlab->SetNextSlab(nullptr);

    if (g_HeadSlab == nullptr) {
        g_HeadSlab = NewSlab;
        g_ActiveSlab = NewSlab;
        RegisterThreadContext();
    }
    else {
        g_ActiveSlab->SetNextSlab(NewSlab);
        g_ActiveSlab = NewSlab;
    }
}

template <typename TContext> void LinearStrategyModule<TContext>::ShutdownModule() noexcept
{
    FlushThreadStats();
    UnregisterThreadContext();

    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    SlabDescriptor* Current = g_HeadSlab;

    while (Current != nullptr) {
        SlabDescriptor* Next = Current->GetNextSlab();
        if (Registry != nullptr) {
            Registry->FreeSlab(Current);
        }
        Current = Next;
    }

    g_HeadSlab = nullptr;
    g_ActiveSlab = nullptr;
}

template <typename TContext> void LinearStrategyModule<TContext>::ShutdownSystem() noexcept
{
    SlabRegistry* Registry = g_SlabRegistry.exchange(nullptr, std::memory_order_acq_rel);
    if (Registry == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    for (SlabDescriptor** ThreadHeadPtr : g_ThreadHeads) {
        if (ThreadHeadPtr != nullptr && *ThreadHeadPtr != nullptr) {
            SlabDescriptor* Current = *ThreadHeadPtr;
            while (Current != nullptr) {
                SlabDescriptor* Next = Current->GetNextSlab();
                Registry->FreeSlab(Current);
                Current = Next;
            }
            *ThreadHeadPtr = nullptr;
        }
    }
    g_ThreadHeads.clear();
}

template <typename TContext> void LinearStrategyModule<TContext>::RegisterThreadContext() noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    g_ThreadHeads.push_back(&g_HeadSlab);
}

template <typename TContext> void LinearStrategyModule<TContext>::UnregisterThreadContext() noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    auto Iterator = std::find(g_ThreadHeads.begin(), g_ThreadHeads.end(), &g_HeadSlab);
    if (Iterator != g_ThreadHeads.end()) {
        g_ThreadHeads.erase(Iterator);
    }
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
    if (m_HasState && m_MarkedSlab != nullptr) {
        LinearStrategyModule<TContext>::RewindState(m_MarkedSlab, m_MarkedOffset);
    }
}

template <typename TContext> void LinearScopedMarker<TContext>::Commit() noexcept
{
    m_HasState = false;
}

} // namespace Allocator
