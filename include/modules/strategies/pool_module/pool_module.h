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
            LOG_ALLOCATOR("DEBUG", "Pool[" << g_ChunkSize
                                           << "B]: Active Slab full or NULL. Searching chain...");

            SlabDescriptor* Current = (tls.FirstNonFullSlab) ? tls.FirstNonFullSlab : tls.HeadSlab;
            while (Current) {
                if (PoolStrategy::CanFit(*Current)) {
                    LOG_ALLOCATOR("DEBUG",
                                  "Pool[" << g_ChunkSize << "B]: Found available slab in chain.");
                    tls.ActiveSlab = Current;
                    tls.FirstNonFullSlab = Current;
                    Result = PoolStrategy::Allocate(*tls.ActiveSlab);
                    break;
                }
                Current = Current->GetNextSlab();
            }

            if (!Result) {
                LOG_ALLOCATOR("DEBUG",
                              "Pool[" << g_ChunkSize << "B]: Chain exhausted. Requesting growth.");
                GrowSlabChain();

                if (tls.ActiveSlab) {
                    LOG_ALLOCATOR("DEBUG",
                                  "Pool[" << g_ChunkSize << "B]: Allocation after growth.");
                    Result = PoolStrategy::Allocate(*tls.ActiveSlab);
                }
                else {
                    LOG_ALLOCATOR("CRITICAL", "Pool[" << g_ChunkSize << "B]: Growth FAILED.");
                }
            }
        }

        if (Result != nullptr) [[likely]] {
            LOG_ALLOCATOR("DEBUG", "Pool[" << g_ChunkSize << "B]: Success. Ptr: " << Result);
            g_Stats.AllocationCount.fetch_add(1, std::memory_order_relaxed);
            g_Stats.BytesAllocated.fetch_add(g_ChunkSize, std::memory_order_relaxed);
            ALLOCATOR_DIAGNOSTIC({ UpdatePeakUsage(); });
        }
        else {
            LOG_ALLOCATOR("ERROR", "Pool[" << g_ChunkSize << "B]: Failed to return memory.");
        }

        return Result;
    }

    static void Free(void* MemoryToFree) noexcept;
    static ContextStats::Snapshot GetStats() noexcept { return g_Stats.GetSnapshot(); }
};

} // namespace Allocator
