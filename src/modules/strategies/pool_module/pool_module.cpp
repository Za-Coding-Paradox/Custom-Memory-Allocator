#include <modules/strategies/pool_module/pool_module.h>

namespace Allocator {

template <typename TContext>
thread_local PoolModuleThreadGuard<TContext> PoolModule<TContext>::g_ThreadGuard;

template <typename TContext>
std::atomic<SlabRegistry*> PoolModule<TContext>::g_SlabRegistry{nullptr};

template <typename TContext> std::mutex PoolModule<TContext>::g_ContextMutex;
template <typename TContext> std::vector<SlabDescriptor**> PoolModule<TContext>::g_ThreadHeads;
template <typename TContext> ContextStats PoolModule<TContext>::g_Stats;

template <typename TContext> void PoolModule<TContext>::Free(void* MemoryToFree) noexcept
{
    if (MemoryToFree == nullptr) [[unlikely]]
        return;

    auto& tls = GetTLS();
    const uintptr_t Target = reinterpret_cast<uintptr_t>(MemoryToFree);
    LOG_ALLOCATOR("DEBUG", "Pool[" << g_ChunkSize << "B]: Freeing ptr " << MemoryToFree);

    if (tls.ActiveSlab) {
        const uintptr_t Start = tls.ActiveSlab->GetSlabStart();
        if (Target >= Start && Target < (Start + g_ConstSlabSize)) [[likely]] {
            PoolStrategy::Free(*tls.ActiveSlab, MemoryToFree);
            g_Stats.BytesFreed.fetch_add(g_ChunkSize, std::memory_order_relaxed);
            return;
        }
    }

    SlabDescriptor* Current = tls.HeadSlab;
    while (Current) {
        const uintptr_t Start = Current->GetSlabStart();
        if (Target >= Start && Target < (Start + g_ConstSlabSize)) {
            PoolStrategy::Free(*Current, MemoryToFree);
            g_Stats.BytesFreed.fetch_add(g_ChunkSize, std::memory_order_relaxed);
            if (tls.ActiveSlab && !PoolStrategy::CanFit(*tls.ActiveSlab)) {
                tls.ActiveSlab = Current;
            }
            return;
        }
        Current = Current->GetNextSlab();
    }
    LOG_ALLOCATOR("ERROR", "Pool[" << g_ChunkSize << "B]: Free failed. Ptr not in pool.");
}

template <typename TContext> void PoolModule<TContext>::GrowSlabChain() noexcept
{
    auto& tls = GetTLS();
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    if (!Registry) {
        LOG_ALLOCATOR("CRITICAL", "Pool[" << g_ChunkSize << "B]: GrowSlabChain: NULL Registry!");
        return;
    }

    SlabDescriptor* NewSlab = Registry->AllocateSlab();
    if (!NewSlab) {
        LOG_ALLOCATOR("ERROR", "Pool[" << g_ChunkSize << "B]: Registry OOM.");
        return;
    }

    PoolStrategy::Format(*NewSlab, g_ChunkSize);

    if (tls.HeadSlab == nullptr) {
        LOG_ALLOCATOR("INFO", "Pool[" << g_ChunkSize << "B]: Initial thread slab.");
        tls.HeadSlab = NewSlab;
        RegisterThreadContext(&tls.HeadSlab);
    }
    else {
        NewSlab->SetNextSlab(tls.HeadSlab);
        tls.HeadSlab = NewSlab;
    }

    tls.ActiveSlab = NewSlab;
    tls.FirstNonFullSlab = NewSlab;
}

template <typename TContext> void PoolModule<TContext>::UpdatePeakUsage() noexcept
{
    const size_t Alloc = g_Stats.BytesAllocated.load(std::memory_order_relaxed);
    const size_t Freed = g_Stats.BytesFreed.load(std::memory_order_relaxed);
    const size_t Current = (Alloc > Freed) ? (Alloc - Freed) : 0;
    size_t Peak = g_Stats.PeakUsage.load(std::memory_order_relaxed);
    while (Current > Peak) {
        if (g_Stats.PeakUsage.compare_exchange_weak(Peak, Current, std::memory_order_relaxed))
            break;
    }
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
void PoolModule<TContext>::RegisterThreadContext(SlabDescriptor** ThreadHeadPtr) noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    g_ThreadHeads.push_back(ThreadHeadPtr);
}

template <typename TContext>
void PoolModule<TContext>::UnregisterThreadContext(SlabDescriptor** ThreadHeadPtr) noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    std::erase(g_ThreadHeads, ThreadHeadPtr);
}

template <typename TContext> void PoolModule<TContext>::ShutdownModule() noexcept
{
    auto& tls = GetTLS();
    LOG_ALLOCATOR("INFO", "Pool[" << g_ChunkSize << "B]: Thread Shutdown.");
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    UnregisterThreadContext(&tls.HeadSlab);

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

template <typename TContext> PoolModuleThreadGuard<TContext>::PoolModuleThreadGuard() noexcept
{
    LOG_ALLOCATOR("DEBUG", "[TLS] Pool Guard Constructed.");
}

template <typename TContext> PoolModuleThreadGuard<TContext>::~PoolModuleThreadGuard()
{
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
