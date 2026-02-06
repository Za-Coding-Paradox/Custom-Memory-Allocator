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

template <typename TContext>
void PoolModule<TContext>::InitializeModule(SlabRegistry* RegistryInstance) noexcept
{
    if (RegistryInstance != nullptr) {
        g_SlabRegistry.store(RegistryInstance, std::memory_order_release);

        g_Stats.BytesAllocated.store(0, std::memory_order_relaxed);
        g_Stats.BytesFreed.store(0, std::memory_order_relaxed);
        g_Stats.AllocationCount.store(0, std::memory_order_relaxed);
        g_Stats.PeakUsage.store(0, std::memory_order_relaxed);
    }
}

template <typename TContext> [[nodiscard]] void* PoolModule<TContext>::Allocate() noexcept
{
    (void)g_ThreadGuard;

    void* AllocatedBlock = nullptr;

    if (g_ActiveSlab != nullptr) [[likely]] {
        AllocatedBlock = PoolStrategy::Allocate(*g_ActiveSlab);
    }

    if (AllocatedBlock == nullptr) {
        SlabDescriptor* CurrentSlab =
            (g_FirstNonFullSlab != nullptr) ? g_FirstNonFullSlab : g_HeadSlab;

        while (CurrentSlab != nullptr) {
            if (PoolStrategy::CanFit(*CurrentSlab)) {
                g_ActiveSlab = CurrentSlab;
                g_FirstNonFullSlab = CurrentSlab;
                AllocatedBlock = PoolStrategy::Allocate(*g_ActiveSlab);
                break;
            }
            if (g_FirstNonFullSlab == CurrentSlab) {
                g_FirstNonFullSlab = CurrentSlab->GetNextSlab();
            }

            CurrentSlab = CurrentSlab->GetNextSlab();
        }
    }

    if (AllocatedBlock == nullptr) {
        GrowSlabChain();
        if (g_ActiveSlab != nullptr) {
            AllocatedBlock = PoolStrategy::Allocate(*g_ActiveSlab);
            g_FirstNonFullSlab = g_ActiveSlab;
        }
    }

    if (AllocatedBlock != nullptr) [[likely]] {
        g_Stats.BytesAllocated.fetch_add(g_ChunkSize, std::memory_order_relaxed);
        g_Stats.AllocationCount.fetch_add(1, std::memory_order_relaxed);
        UpdatePeakUsage();
    }

    return AllocatedBlock;
}

template <typename TContext> void PoolModule<TContext>::Free(void* MemoryToFree) noexcept
{
    if (MemoryToFree == nullptr) [[unlikely]]
        return;

    const auto TargetAddress = std::bit_cast<uintptr_t>(MemoryToFree);
    SlabDescriptor* CurrentSlab = g_HeadSlab;

    while (CurrentSlab != nullptr) {
        const uintptr_t SlabStart = CurrentSlab->GetSlabStart();
        const uintptr_t SlabEnd = SlabStart + g_ConstSlabSize;

        if (TargetAddress >= SlabStart && TargetAddress < SlabEnd) [[likely]] {
            PoolStrategy::Free(*CurrentSlab, MemoryToFree);

            g_Stats.BytesFreed.fetch_add(g_ChunkSize, std::memory_order_relaxed);

            if (g_ActiveSlab == nullptr || !PoolStrategy::CanFit(*g_ActiveSlab)) {
                g_ActiveSlab = CurrentSlab;
            }
            return;
        }
        CurrentSlab = CurrentSlab->GetNextSlab();
    }
}

template <typename TContext> void PoolModule<TContext>::GrowSlabChain() noexcept
{
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    if (Registry == nullptr) [[unlikely]] {
        return;
    }

    SlabDescriptor* NewSlab = Registry->AllocateSlab();
    if (NewSlab == nullptr) [[unlikely]] {
        return;
    }

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
}

template <typename TContext> void PoolModule<TContext>::UpdatePeakUsage() noexcept
{
    const size_t Allocated = g_Stats.BytesAllocated.load(std::memory_order_relaxed);
    const size_t Freed = g_Stats.BytesFreed.load(std::memory_order_relaxed);
    const size_t CurrentUsage = (Allocated > Freed) ? (Allocated - Freed) : static_cast<size_t>(0);

    size_t CurrentPeak = g_Stats.PeakUsage.load(std::memory_order_relaxed);

    while (CurrentUsage > CurrentPeak) {
        if (g_Stats.PeakUsage.compare_exchange_weak(CurrentPeak, CurrentUsage,
                                                    std::memory_order_relaxed)) {
            break;
        }
    }
}

template <typename TContext> void PoolModule<TContext>::RegisterThreadContext() noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    g_ThreadHeads.push_back(&g_HeadSlab);
}

template <typename TContext> void PoolModule<TContext>::UnregisterThreadContext() noexcept
{
    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    auto Iterator = std::find(g_ThreadHeads.begin(), g_ThreadHeads.end(), &g_HeadSlab);
    if (Iterator != g_ThreadHeads.end()) {
        g_ThreadHeads.erase(Iterator);
    }
}

template <typename TContext> void PoolModule<TContext>::ShutdownModule() noexcept
{
    SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
    UnregisterThreadContext();

    SlabDescriptor* CurrentSlab = g_HeadSlab;
    while (CurrentSlab != nullptr) {
        SlabDescriptor* NextSlab = CurrentSlab->GetNextSlab();
        if (Registry != nullptr) {
            Registry->FreeSlab(CurrentSlab);
        }
        CurrentSlab = NextSlab;
    }

    g_HeadSlab = nullptr;
    g_ActiveSlab = nullptr;
    g_FirstNonFullSlab = nullptr;
}

template <typename TContext> void PoolModule<TContext>::ShutdownSystem() noexcept
{
    SlabRegistry* Registry = g_SlabRegistry.exchange(nullptr, std::memory_order_acq_rel);
    if (Registry == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> Lock(g_ContextMutex);
    for (SlabDescriptor** ThreadHeadPtr : g_ThreadHeads) {
        if (ThreadHeadPtr != nullptr && *ThreadHeadPtr != nullptr) {
            SlabDescriptor* CurrentSlab = *ThreadHeadPtr;
            while (CurrentSlab != nullptr) {
                SlabDescriptor* NextSlab = CurrentSlab->GetNextSlab();
                Registry->FreeSlab(CurrentSlab);
                CurrentSlab = NextSlab;
            }
            *ThreadHeadPtr = nullptr;
        }
    }
    g_ThreadHeads.clear();
}

template <typename TContext> PoolModuleThreadGuard<TContext>::~PoolModuleThreadGuard()
{
    PoolModule<TContext>::ShutdownModule();
}

} // namespace Allocator

namespace Allocator {

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
