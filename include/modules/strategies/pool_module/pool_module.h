#include <modules/allocation_stats.h>
#include <modules/strategies/pool_module/pool_strategy.h>

namespace Allocator {

template <typename TContext> struct PoolModuleThreadGuard
{
    PoolModuleThreadGuard() noexcept = default;
    ~PoolModuleThreadGuard();
};

template <typename TContext> class PoolModule
{
public:
    static constexpr size_t g_ChunkSize = TContext::g_BucketSize;

    PoolModule() = delete;
    ~PoolModule() = delete;

    static void InitializeModule(SlabRegistry* RegistryInstance) noexcept;
    static void ShutdownModule() noexcept;
    static void ShutdownSystem() noexcept;

    [[nodiscard]] __attribute__((always_inline)) static void* Allocate() noexcept
    {
        void* Result = nullptr;

        if (g_ActiveSlab && PoolStrategy::CanFit(*g_ActiveSlab)) [[likely]] {
            Result = PoolStrategy::Allocate(*g_ActiveSlab);
        }
        else {
            SlabDescriptor* Current = (g_FirstNonFullSlab) ? g_FirstNonFullSlab : g_HeadSlab;
            while (Current) {
                if (PoolStrategy::CanFit(*Current)) {
                    g_ActiveSlab = Current;
                    g_FirstNonFullSlab = Current;
                    Result = PoolStrategy::Allocate(*g_ActiveSlab);
                    break;
                }
                Current = Current->GetNextSlab();
            }

            if (!Result) {
                GrowSlabChain();
                if (g_ActiveSlab)
                    Result = PoolStrategy::Allocate(*g_ActiveSlab);
            }
        }

        if (Result != nullptr) [[likely]] {
            g_Stats.AllocationCount.fetch_add(1, std::memory_order_relaxed);
            g_Stats.BytesAllocated.fetch_add(g_ChunkSize, std::memory_order_relaxed);

            ALLOCATOR_DIAGNOSTIC({ UpdatePeakUsage(); });
        }
        return Result;
    }

    static void Free(void* MemoryToFree) noexcept;
    static ContextStats::Snapshot GetStats() noexcept { return g_Stats.GetSnapshot(); }

private:
    static void GrowSlabChain() noexcept;
    static void RegisterThreadContext() noexcept;
    static void UnregisterThreadContext() noexcept;
    static void UpdatePeakUsage() noexcept;

    static thread_local SlabDescriptor* g_ActiveSlab;
    static thread_local SlabDescriptor* g_HeadSlab;
    static thread_local SlabDescriptor* g_FirstNonFullSlab;
    static thread_local PoolModuleThreadGuard<TContext> g_ThreadGuard;

    static std::atomic<SlabRegistry*> g_SlabRegistry;
    static std::mutex g_ContextMutex;
    static std::vector<SlabDescriptor**> g_ThreadHeads;
    static ContextStats g_Stats;
};

} // namespace Allocator
