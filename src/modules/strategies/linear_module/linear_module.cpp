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

template <typename TContext> static constexpr size_t g_SlabBatchSize = 8;

template <typename TContext>
void* LinearStrategyModule<TContext>::OverFlowAllocate(size_t AllocationSize,
                                                       size_t AllocationAlignment) noexcept
{
    if (AllocationSize > g_ConstSlabSize) [[unlikely]] {
        LOG_ALLOCATOR("ERROR", "[L-OVERFLOW] AllocationSize "
                                   << AllocationSize << " exceeds SlabSize. Cannot satisfy.");
        return nullptr;
    }

    auto& tls = GetTLS();

    static constexpr int kMaxAdvances = 4;

    for (int Attempt = 0; Attempt < kMaxAdvances; ++Attempt) {
        SlabDescriptor* NextSlab = tls.ActiveSlab->GetNextSlab();

        if (NextSlab != nullptr) {
            LOG_ALLOCATOR("DEBUG", "[L-OVERFLOW] Attempt "
                                       << Attempt << ": Advancing to existing slab " << NextSlab);
            NextSlab->UpdateFreeListHead(NextSlab->GetSlabStart());
            tls.ActiveSlab = NextSlab;
        }
        else {
            LOG_ALLOCATOR("DEBUG", "[L-OVERFLOW] Attempt "
                                       << Attempt << ": Chain exhausted. Calling GrowSlabChain.");
            SlabDescriptor* const OldActive = tls.ActiveSlab;
            GrowSlabChain();

            if (tls.ActiveSlab == OldActive) [[unlikely]] {
                LOG_ALLOCATOR("CRITICAL",
                              "[L-OVERFLOW] GrowSlabChain failed to produce a new slab. OOM.");
                return nullptr;
            }
        }

        if (LinearStrategy::CanFit(*tls.ActiveSlab, AllocationSize, AllocationAlignment)) {
            void* Result =
                LinearStrategy::Allocate(*tls.ActiveSlab, AllocationSize, AllocationAlignment);

            if (Result != nullptr) [[likely]] {
                LOG_ALLOCATOR("DEBUG", "[L-OVERFLOW] Satisfied after "
                                           << Attempt + 1 << " advance(s). Ptr: " << Result);
                return Result;
            }
        }

        LOG_ALLOCATOR("WARN", "[L-OVERFLOW] Slab "
                                  << tls.ActiveSlab << " still cannot fit " << AllocationSize
                                  << "B @ align=" << AllocationAlignment << ". Advancing again.");
    }

    LOG_ALLOCATOR("ERROR", "[L-OVERFLOW] Exhausted "
                               << kMaxAdvances << " slab advances for " << AllocationSize
                               << "B @ align=" << AllocationAlignment << ". Returning nullptr.");
    return nullptr;
}

template <typename TContext> void LinearStrategyModule<TContext>::GrowSlabChain() noexcept
{
    auto& tls = GetTLS();

    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    if (!Registry) {
        LOG_ALLOCATOR("CRITICAL", "LinearModule: Growth failed - Registry is NULL.");
        return;
    }

    SlabDescriptor* Batch[g_SlabBatchSize<TContext>];
    const size_t Got = Registry->AllocateSlabBatch(g_SlabBatchSize<TContext>, Batch);

    if (Got == 0) {
        LOG_ALLOCATOR("ERROR", "LinearModule: Registry returned 0 slabs. OOM.");
        return;
    }

    LOG_ALLOCATOR("DEBUG", "LinearModule: Acquired batch of " << Got << " slabs.");

    for (size_t i = 0; i < Got - 1; ++i)
        Batch[i]->SetNextSlab(Batch[i + 1]);
    Batch[Got - 1]->SetNextSlab(nullptr);

    if (!tls.HeadSlab) {
        tls.HeadSlab = Batch[0];
        tls.ActiveSlab = Batch[0];
        RegisterThreadContext(&tls.HeadSlab);
    }
    else {
        tls.ActiveSlab->SetNextSlab(Batch[0]);
        tls.ActiveSlab = Batch[0];
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
    ALLOCATOR_DIAGNOSTIC({ FlushThreadStats(); });

    auto& tls = GetTLS();
    if (!tls.HeadSlab)
        return;

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
            SlabDescriptor* NextSlab = Current->GetNextSlab();
            LinearStrategy::Reset(*Current);
            Current->SetNextSlab(NextSlab);
            Current = NextSlab;
        }
        tls.ActiveSlab = tls.HeadSlab;

        ALLOCATOR_DIAGNOSTIC({ FlushThreadStats(); });
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

    if (!tls.ActiveSlab) [[unlikely]] {
        LOG_ALLOCATOR(
            "DEBUG",
            "[L-STATE] GetCurrentState called on uninitialized thread. Returning null state.");
        return {nullptr, 0};
    }

    uintptr_t CurrentOffset = tls.ActiveSlab->GetFreeListHead();
    LOG_ALLOCATOR("DEBUG", "[L-STATE] Captured State: Slab [" << tls.ActiveSlab << "] Offset ["
                                                              << CurrentOffset << "]");
    return {tls.ActiveSlab, CurrentOffset};
}

template <typename TContext>
void LinearStrategyModule<TContext>::RewindState(SlabDescriptor* SavedSlab,
                                                 uintptr_t SavedOffset) noexcept
    requires(TContext::IsRewindable)
{
    if (!SavedSlab) [[unlikely]] {
        LOG_ALLOCATOR("WARN", "[L-REWIND] Received null SavedSlab. Performing full thread reset.");

        auto& tls = GetTLS();
        if (tls.HeadSlab != nullptr) {
            SlabDescriptor* Current = tls.HeadSlab;
            while (Current) {
                SlabDescriptor* NextSlab = Current->GetNextSlab();
                LinearStrategy::Reset(*Current);
                Current->SetNextSlab(NextSlab);
                Current = NextSlab;
            }
            tls.ActiveSlab = tls.HeadSlab;
            ALLOCATOR_DIAGNOSTIC({ FlushThreadStats(); });
        }
        return;
    }

    auto& tls = GetTLS();

    ALLOCATOR_DIAGNOSTIC({
        size_t UsedBefore = GetThreadTotalUsed();
        LOG_ALLOCATOR("DEBUG", "[L-REWIND] Starting Rewind. Usage before: " << UsedBefore);
        LOG_ALLOCATOR("DEBUG", "[L-REWIND] Target: Slab [" << SavedSlab << "] Offset ["
                                                           << SavedOffset << "]");
    });

    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    SlabDescriptor* Victim = SavedSlab->GetNextSlab();

    if (Victim != nullptr) {
        LOG_ALLOCATOR("DEBUG", "[L-REWIND] Overflow detected. Detaching trailing slab chain.");

        SavedSlab->SetNextSlab(nullptr);
        tls.ActiveSlab = SavedSlab;

        while (Victim != nullptr) {
            SlabDescriptor* NextVictim = Victim->GetNextSlab();
            if (Registry) [[likely]]
                Registry->FreeSlab(Victim);
            Victim = NextVictim;
        }
    }
    else {
        tls.ActiveSlab = SavedSlab;
    }

    LinearStrategy::RewindToMarker(*tls.ActiveSlab, SavedOffset);

    ALLOCATOR_DIAGNOSTIC({
        size_t UsedAfter = GetThreadTotalUsed();
        LOG_ALLOCATOR("DEBUG", "[L-REWIND] Rewind complete. Usage after: " << UsedAfter);
    });
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
