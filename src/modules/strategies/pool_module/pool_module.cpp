#include <modules/strategies/pool_module/pool_module.h>

namespace Allocator {

template <typename TContext>
thread_local PoolModuleThreadGuard<TContext> PoolModule<TContext>::g_ThreadGuard;

template <typename TContext>
std::atomic<SlabRegistry*> PoolModule<TContext>::g_SlabRegistry{nullptr};

template <typename TContext> std::mutex PoolModule<TContext>::g_ContextMutex;
template <typename TContext>
std::vector<typename PoolModule<TContext>::ThreadLocalData*> PoolModule<TContext>::g_ThreadHeads;
template <typename TContext> ContextStats PoolModule<TContext>::g_Stats;

template <typename TContext> static constexpr size_t g_PoolSlabBatchSize = 4;

template <typename TContext> void PoolModule<TContext>::GrowSlabChain() noexcept
{
    auto& tls = GetTLS();

    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    if (!Registry) {
        LOG_ALLOCATOR("CRITICAL", "Pool[" << g_ChunkSize << "B]: GrowSlabChain: NULL Registry!");
        return;
    }

    SlabDescriptor* Batch[g_PoolSlabBatchSize<TContext>];
    const size_t Got = Registry->AllocateSlabBatch(g_PoolSlabBatchSize<TContext>, Batch);

    if (Got == 0) {
        LOG_ALLOCATOR("ERROR", "Pool[" << g_ChunkSize << "B]: Registry returned 0 slabs. OOM.");
        return;
    }

    LOG_ALLOCATOR("DEBUG", "Pool[" << g_ChunkSize << "B]: Acquired batch of " << Got << " slabs.");

    for (size_t i = 0; i < Got; ++i)
        PoolStrategy::Format(*Batch[i], g_ChunkSize);

    for (size_t i = 0; i < Got - 1; ++i)
        Batch[i]->SetNextSlab(Batch[i + 1]);

    if (tls.HeadSlab == nullptr) {
        Batch[Got - 1]->SetNextSlab(nullptr);
        tls.HeadSlab = Batch[0];
        RegisterThreadContext(&tls);
    }
    else {
        Batch[Got - 1]->SetNextSlab(tls.HeadSlab);
        tls.HeadSlab = Batch[0];
    }

    tls.ActiveSlab = Batch[0];
    tls.FirstNonFullSlab = Batch[0];
}

template <typename TContext> void PoolModule<TContext>::UpdatePeakUsage() noexcept
{
    ALLOCATOR_DIAGNOSTIC({
        size_t Allocated = g_Stats.BytesAllocated.load(std::memory_order_relaxed);
        size_t Freed = g_Stats.BytesFreed.load(std::memory_order_relaxed);
        size_t Current = (Allocated > Freed) ? (Allocated - Freed) : 0;
        size_t Peak = g_Stats.PeakUsage.load(std::memory_order_relaxed);

        while (Current > Peak &&
               !g_Stats.PeakUsage.compare_exchange_weak(Peak, Current, std::memory_order_relaxed))
            ;
    });
}

template <typename TContext>
void PoolModule<TContext>::InitializeModule(SlabRegistry* RegistryInstance) noexcept
{
    if (RegistryInstance) {
        LOG_ALLOCATOR("INFO", "Pool[" << g_ChunkSize << "B]: Init at " << RegistryInstance);
        g_SlabRegistry.store(RegistryInstance, std::memory_order_release);
    }
    else {
        LOG_ALLOCATOR("CRITICAL", "Pool[" << g_ChunkSize << "B]: Init NULL!");
    }
}

template <typename TContext>
void PoolModule<TContext>::RegisterThreadContext(ThreadLocalData* TLS) noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    g_ThreadHeads.push_back(TLS);
}

template <typename TContext>
void PoolModule<TContext>::UnregisterThreadContext(ThreadLocalData* TLS) noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    std::erase(g_ThreadHeads, TLS);
}

template <typename TContext> void PoolModule<TContext>::ShutdownModule() noexcept
{
    ALLOCATOR_DIAGNOSTIC({ FlushThreadStats(); });

    auto& tls = GetTLS();
    LOG_ALLOCATOR("INFO", "Pool[" << g_ChunkSize << "B]: Thread Shutdown.");

    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    UnregisterThreadContext(&tls);

    SlabDescriptor* Current = tls.HeadSlab;
    while (Current) {
        SlabDescriptor* Next = Current->GetNextSlab();
        if (Registry)
            Registry->FreeSlab(Current);
        Current = Next;
    }
    tls.HeadSlab = tls.ActiveSlab = tls.FirstNonFullSlab = nullptr;
}

template <typename TContext> void PoolModule<TContext>::ShutdownSystem() noexcept
{
    LOG_ALLOCATOR("SYSTEM", "Pool[" << g_ChunkSize << "B]: Global Shutdown.");
    SlabRegistry* Registry = g_SlabRegistry.exchange(nullptr, std::memory_order_acq_rel);
    if (!Registry)
        return;

    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    for (ThreadLocalData* TLSEntry : g_ThreadHeads) {
        if (!TLSEntry)
            continue;

        SlabDescriptor* Current = TLSEntry->HeadSlab;
        while (Current) {
            SlabDescriptor* Next = Current->GetNextSlab();
            Registry->FreeSlab(Current);
            Current = Next;
        }

        TLSEntry->HeadSlab = nullptr;
        TLSEntry->ActiveSlab = nullptr;
        TLSEntry->FirstNonFullSlab = nullptr;
    }
    g_ThreadHeads.clear();
}

template <typename TContext> PoolModuleThreadGuard<TContext>::PoolModuleThreadGuard() noexcept
{
    LOG_ALLOCATOR("DEBUG", "[TLS] Pool Guard Constructed.");
}

template <typename TContext> PoolModuleThreadGuard<TContext>::~PoolModuleThreadGuard()
{
    PoolModule<TContext>::FlushThreadStats();
    PoolModule<TContext>::ShutdownModule();
}

template class PoolModule<BucketScope<16>>;
template class PoolModule<BucketScope<32>>;
template class PoolModule<BucketScope<64>>;
template class PoolModule<BucketScope<128>>;
template class PoolModule<BucketScope<256>>;

template struct PoolModuleThreadGuard<BucketScope<16>>;
template struct PoolModuleThreadGuard<BucketScope<32>>;
template struct PoolModuleThreadGuard<BucketScope<64>>;
template struct PoolModuleThreadGuard<BucketScope<128>>;
template struct PoolModuleThreadGuard<BucketScope<256>>;

} // namespace Allocator
