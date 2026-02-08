#include <iomanip>
#include <modules/allocator_engine.h>

namespace Allocator {

namespace {
constexpr uint32_t g_DefaultHandleCapacity = 4096;
constexpr size_t g_BytesPerKB = 1024;
constexpr size_t g_BytesPerMB = 1024 * 1024;
} // namespace

AllocatorEngine::AllocatorEngine(size_t SlabSize, size_t ArenaSize)
    : m_Registry(SlabSize, ArenaSize), m_HandleTable(g_DefaultHandleCapacity)
{
    LOG_ALLOCATOR("INFO", "Engine: Constructor called. Registry Address: " << &m_Registry);
}

AllocatorEngine::~AllocatorEngine()
{
    LOG_ALLOCATOR("INFO", "Engine: Destructor called.");
    Shutdown();
}

void AllocatorEngine::Initialize()
{
    LOG_ALLOCATOR("SYSTEM", "Engine: Initializing Static Strategy Modules...");

    // Check registry sanity before passing it
    if (m_Registry.GetArenaSlabsStart() == nullptr) {
        LOG_ALLOCATOR(
            "CRITICAL",
            "Engine: Registry has NULL Slabs Start! Initialization will likely cause Segfault.");
    }

    // Handover to Linear Modules
    LOG_ALLOCATOR("DEBUG",
                  "Engine: Initializing Linear Strategy Modules with Registry: " << &m_Registry);
    LinearStrategyModule<FrameLoad>::InitializeModule(&m_Registry);
    LinearStrategyModule<LevelLoad>::InitializeModule(&m_Registry);
    LinearStrategyModule<GlobalLoad>::InitializeModule(&m_Registry);

    // Handover to Pool Modules
    LOG_ALLOCATOR("DEBUG", "Engine: Initializing Pool Modules...");
    PoolModule<BucketScope<16>>::InitializeModule(&m_Registry);
    PoolModule<BucketScope<32>>::InitializeModule(&m_Registry);
    PoolModule<BucketScope<64>>::InitializeModule(&m_Registry);
    PoolModule<BucketScope<128>>::InitializeModule(&m_Registry);
    PoolModule<BucketScope<256>>::InitializeModule(&m_Registry);

    LOG_ALLOCATOR("SYSTEM", "Engine: Static Handover Complete.");
}

void AllocatorEngine::Shutdown()
{
    LOG_ALLOCATOR("SYSTEM", "Engine: Starting Shutdown...");

    LinearStrategyModule<FrameLoad>::ShutdownSystem();
    LinearStrategyModule<LevelLoad>::ShutdownSystem();
    LinearStrategyModule<GlobalLoad>::ShutdownSystem();

    PoolModule<BucketScope<16>>::ShutdownSystem();
    PoolModule<BucketScope<32>>::ShutdownSystem();
    PoolModule<BucketScope<64>>::ShutdownSystem();
    PoolModule<BucketScope<128>>::ShutdownSystem();
    PoolModule<BucketScope<256>>::ShutdownSystem();

    LOG_ALLOCATOR("DEBUG", "Engine: Clearing Handle Table...");
    m_HandleTable.Clear();

    LOG_ALLOCATOR("SYSTEM", "Engine: Shutdown Complete.");
}

template <typename TScope> void AllocatorEngine::PrintStats(const char* ScopeName) const noexcept
{
    ContextStats::Snapshot Snap;

    LOG_ALLOCATOR("DEBUG", "Engine: Collecting stats for " << ScopeName);

    if constexpr (ScopeTraits<TScope>::SupportsHandles) {
        using Bucket = typename PoolMap<TScope::g_BucketSize>::Type;
        Snap = PoolModule<Bucket>::GetStats();
    }
    else {
        LinearStrategyModule<TScope>::FlushThreadStats();
        Snap = LinearStrategyModule<TScope>::GetGlobalStats().GetSnapshot();
    }

    auto FormatBytes = [](size_t Bytes) -> std::string {
        std::stringstream ss;
        if (Bytes < g_BytesPerKB)
            ss << Bytes << " B";
        else if (Bytes < g_BytesPerMB)
            ss << std::fixed << std::setprecision(2) << (double)Bytes / g_BytesPerKB << " KB";
        else
            ss << std::fixed << std::setprecision(2) << (double)Bytes / g_BytesPerMB << " MB";
        return ss.str();
    };

    std::cout << "\n[" << ScopeName << "]\n"
              << "  Allocated : " << FormatBytes(Snap.BytesAllocated) << "\n"
              << "  Current   : " << FormatBytes(Snap.BytesAllocated - Snap.BytesFreed) << "\n"
              << "  Peak      : " << FormatBytes(Snap.PeakUsage) << "\n"
              << "  Count     : " << Snap.AllocationCount << "\n";
}

void AllocatorEngine::GenerateFullReport() const noexcept
{
    std::cout << "================================================================\n"
              << "                    ALLOCATOR PERFORMANCE REPORT                \n"
              << "================================================================\n";

    PrintStats<FrameLoad>("Linear: Frame");
    PrintStats<LevelLoad>("Linear: Level");
    PrintStats<GlobalLoad>("Linear: Global");

    std::cout << "\n--- Fixed Size Pool Buckets ---";
    PrintStats<BucketScope<16>>("Pool: 16B");
    PrintStats<BucketScope<32>>("Pool: 32B");
    PrintStats<BucketScope<64>>("Pool: 64B");
    PrintStats<BucketScope<128>>("Pool: 128B");
    PrintStats<BucketScope<256>>("Pool: 256B");

    std::cout << "\n[Registry State]\n"
              << "  Handles Active: " << m_HandleTable.GetActiveCount() << " / "
              << m_HandleTable.GetCapacity() << "\n"
              << "================================================================\n";
}

template void AllocatorEngine::PrintStats<FrameLoad>(const char*) const noexcept;
template void AllocatorEngine::PrintStats<LevelLoad>(const char*) const noexcept;
template void AllocatorEngine::PrintStats<GlobalLoad>(const char*) const noexcept;
template void AllocatorEngine::PrintStats<BucketScope<16>>(const char*) const noexcept;
template void AllocatorEngine::PrintStats<BucketScope<32>>(const char*) const noexcept;
template void AllocatorEngine::PrintStats<BucketScope<64>>(const char*) const noexcept;
template void AllocatorEngine::PrintStats<BucketScope<128>>(const char*) const noexcept;
template void AllocatorEngine::PrintStats<BucketScope<256>>(const char*) const noexcept;

} // namespace Allocator
