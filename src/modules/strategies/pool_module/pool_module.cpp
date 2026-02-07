#include <modules/strategies/pool_module/pool_module.h>

namespace Allocator {

template <typename TContext>
thread_local SlabDescriptor* PoolModule<TContext>::g_ActiveSlab = nullptr;

template <typename TContext>
thread_local SlabDescriptor* PoolModule<TContext>::g_HeadSlab = nullptr;

template <typename TContext>
thread_local SlabDescriptor* PoolModule<TContext>::g_FirstNonFullSlab = nullptr;

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

    const uintptr_t Target = reinterpret_cast<uintptr_t>(MemoryToFree);
    if (g_ActiveSlab) {
        const uintptr_t Start = g_ActiveSlab->GetSlabStart();
        if (Target >= Start && Target < (Start + g_ConstSlabSize)) [[likely]] {
            PoolStrategy::Free(*g_ActiveSlab, MemoryToFree);
            g_Stats.BytesFreed.fetch_add(g_ChunkSize, std::memory_order_relaxed);
            return;
        }
    }

    SlabDescriptor* Current = g_HeadSlab;
    while (Current) {
        const uintptr_t Start = Current->GetSlabStart();
        if (Target >= Start && Target < (Start + g_ConstSlabSize)) {
            PoolStrategy::Free(*Current, MemoryToFree);
            g_Stats.BytesFreed.fetch_add(g_ChunkSize, std::memory_order_relaxed);

            if (!PoolStrategy::CanFit(*g_ActiveSlab))
                g_ActiveSlab = Current;
            return;
        }
        Current = Current->GetNextSlab();
    }
}

template <typename TContext> void PoolModule<TContext>::GrowSlabChain() noexcept
{
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    if (!Registry) [[unlikely]]
        return;

    SlabDescriptor* NewSlab = Registry->AllocateSlab();
    if (!NewSlab) [[unlikely]]
        return;

    PoolStrategy::Format(*NewSlab, g_ChunkSize);

    if (g_HeadSlab == nullptr) {
        g_HeadSlab = NewSlab;
        RegisterThreadContext();
    }
    else {
        NewSlab->SetNextSlab(g_HeadSlab);
        g_HeadSlab = NewSlab;
    }

    g_ActiveSlab = NewSlab;
    g_FirstNonFullSlab = NewSlab;
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
    if (RegistryInstance)
        g_SlabRegistry.store(RegistryInstance, std::memory_order_release);
}

template <typename TContext> void PoolModule<TContext>::RegisterThreadContext() noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    g_ThreadHeads.push_back(&g_HeadSlab);
}

template <typename TContext> void PoolModule<TContext>::UnregisterThreadContext() noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    std::erase(g_ThreadHeads, &g_HeadSlab);
}

template <typename TContext> void PoolModule<TContext>::ShutdownModule() noexcept
{
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    UnregisterThreadContext();

    SlabDescriptor* Current = g_HeadSlab;
    while (Current) {
        SlabDescriptor* Next = Current->GetNextSlab();
        if (Registry)
            Registry->FreeSlab(Current);
        Current = Next;
    }
    g_HeadSlab = g_ActiveSlab = g_FirstNonFullSlab = nullptr;
}

template <typename TContext> void PoolModule<TContext>::ShutdownSystem() noexcept
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
