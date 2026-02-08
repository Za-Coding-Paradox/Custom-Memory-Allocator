#pragma once

#include <modules/allocation_stats.h>
#include <modules/strategies/pool_module/pool_strategy.h>

namespace Allocator {

template <typename TContext> struct PoolModuleThreadGuard
{
    PoolModuleThreadGuard() noexcept;
    ~PoolModuleThreadGuard();
};

template <typename TContext> class PoolModule
{
public:
    static constexpr size_t g_ChunkSize = TContext::g_BucketSize;

    struct ThreadLocalData
    {
        SlabDescriptor* ActiveSlab = nullptr;
        SlabDescriptor* HeadSlab = nullptr;
        SlabDescriptor* FirstNonFullSlab = nullptr;

        size_t BytesAllocated = 0;
        size_t BytesFreed = 0;
        size_t AllocCount = 0;
        size_t FreeCount = 0;
    };

private:
    static ThreadLocalData& GetTLS() noexcept
    {
        static thread_local ThreadLocalData data;
        return data;
    }

    static thread_local PoolModuleThreadGuard<TContext> g_ThreadGuard;
    static std::atomic<SlabRegistry*> g_SlabRegistry;
    static std::mutex g_ContextMutex;
    static std::vector<SlabDescriptor**> g_ThreadHeads;
    static ContextStats g_Stats;

    static void GrowSlabChain() noexcept;
    static void RegisterThreadContext(SlabDescriptor** ThreadHeadPtr) noexcept;
    static void UnregisterThreadContext(SlabDescriptor** ThreadHeadPtr) noexcept;
    static void UpdatePeakUsage() noexcept;

    friend struct PoolModuleThreadGuard<TContext>;

public:
    PoolModule() = delete;
    ~PoolModule() = delete;

    static void InitializeModule(SlabRegistry* RegistryInstance) noexcept;
    static void ShutdownModule() noexcept;
    static void ShutdownSystem() noexcept;

    static inline void FlushThreadStats() noexcept
    {
        ALLOCATOR_DIAGNOSTIC({
            auto& tls = GetTLS();
            if (tls.BytesAllocated > 0 || tls.BytesFreed > 0 || tls.AllocCount > 0 ||
                tls.FreeCount > 0) {
                if (tls.BytesAllocated > 0)
                    g_Stats.BytesAllocated.fetch_add(tls.BytesAllocated, std::memory_order_relaxed);
                if (tls.BytesFreed > 0)
                    g_Stats.BytesFreed.fetch_add(tls.BytesFreed, std::memory_order_relaxed);
                if (tls.AllocCount > 0)
                    g_Stats.AllocationCount.fetch_add(tls.AllocCount, std::memory_order_relaxed);
                if (tls.FreeCount > 0)
                    g_Stats.AllocationCount.fetch_sub(tls.FreeCount, std::memory_order_relaxed);

                tls.BytesAllocated = 0;
                tls.BytesFreed = 0;
                tls.AllocCount = 0;
                tls.FreeCount = 0;
                UpdatePeakUsage();
            }
        });
    }

    [[nodiscard]] __attribute__((always_inline)) static void* Allocate() noexcept
    {
        (void)g_ThreadGuard;
        auto& tls = GetTLS();
        void* Result = nullptr;

        if (tls.ActiveSlab && PoolStrategy::CanFit(*tls.ActiveSlab)) [[likely]] {
            LOG_ALLOCATOR("DEBUG", "Pool[" << g_ChunkSize << "B]: Allocating from Active Slab.");
            Result = PoolStrategy::Allocate(*tls.ActiveSlab);
        }
        else {
            LOG_ALLOCATOR("DEBUG", "Pool[" << g_ChunkSize << "B]: Searching chain...");
            SlabDescriptor* Current = (tls.FirstNonFullSlab) ? tls.FirstNonFullSlab : tls.HeadSlab;
            while (Current) {
                if (PoolStrategy::CanFit(*Current)) {
                    tls.ActiveSlab = Current;
                    tls.FirstNonFullSlab = Current;
                    Result = PoolStrategy::Allocate(*tls.ActiveSlab);
                    break;
                }
                Current = Current->GetNextSlab();
            }

            if (!Result) {
                LOG_ALLOCATOR("DEBUG", "Pool[" << g_ChunkSize << "B]: Chain exhausted. Growing.");
                GrowSlabChain();
                if (tls.ActiveSlab)
                    Result = PoolStrategy::Allocate(*tls.ActiveSlab);
            }
        }

        if (Result != nullptr) [[likely]] {
            ALLOCATOR_DIAGNOSTIC({
                tls.AllocCount++;
                tls.BytesAllocated += g_ChunkSize;
            });
        }
        else {
            LOG_ALLOCATOR("ERROR", "Pool[" << g_ChunkSize << "B]: Failed to return memory.");
        }
        return Result;
    }

    static void Free(void* MemoryToFree) noexcept
    {
        if (MemoryToFree == nullptr) [[unlikely]]
            return;

        SlabRegistry* Registry = g_SlabRegistry.load(std::memory_order_acquire);
        if (!Registry) [[unlikely]]
            return;

        SlabDescriptor* Slab = Registry->GetSlabDescriptor(MemoryToFree);

        if (Slab) [[likely]] {
            PoolStrategy::Free(*Slab, MemoryToFree);

            ALLOCATOR_DIAGNOSTIC({
                auto& tls = GetTLS();
                tls.BytesFreed += g_ChunkSize;
                tls.FreeCount++;
            });
        }
        else {
            LOG_ALLOCATOR("ERROR",
                          "Pool: Attempted to free invalid ptr (Not in Arena): " << MemoryToFree);
        }
    }

    static ContextStats::Snapshot GetStats() noexcept { return g_Stats.GetSnapshot(); }
};

} // namespace Allocator
