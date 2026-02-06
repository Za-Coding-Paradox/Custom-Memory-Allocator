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

    [[nodiscard]] static void* Allocate() noexcept;
    static void Free(void* MemoryToFree) noexcept;

    static ContextStats::Snapshot GetStats() noexcept { return g_Stats.GetSnapshot(); }

private:
    static void GrowSlabChain() noexcept;
    static void RegisterThreadContext() noexcept;
    static void UnregisterThreadContext() noexcept;
    static void UpdatePeakUsage() noexcept;

    static thread_local SlabDescriptor* g_ActiveSlab;
    static thread_local SlabDescriptor* g_HeadSlab;
    static thread_local PoolModuleThreadGuard<TContext> g_ThreadGuard;

    static std::atomic<SlabRegistry*> g_SlabRegistry;
    static std::mutex g_ContextMutex;
    static std::vector<SlabDescriptor**> g_ThreadHeads;
    static ContextStats g_Stats;
};

} // namespace Allocator
