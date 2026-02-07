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
{}

AllocatorEngine::~AllocatorEngine()
{
    Shutdown();
}

void AllocatorEngine::Initialize()
{
    LinearStrategyModule<FrameLoad>::InitializeModule(&m_Registry);
    LinearStrategyModule<LevelLoad>::InitializeModule(&m_Registry);
    LinearStrategyModule<GlobalLoad>::InitializeModule(&m_Registry);

    PoolModule<BucketScope<16>>::InitializeModule(&m_Registry);
    PoolModule<BucketScope<32>>::InitializeModule(&m_Registry);
    PoolModule<BucketScope<64>>::InitializeModule(&m_Registry);
    PoolModule<BucketScope<128>>::InitializeModule(&m_Registry);
    PoolModule<BucketScope<256>>::InitializeModule(&m_Registry);
}

void AllocatorEngine::Shutdown()
{
    LinearStrategyModule<FrameLoad>::ShutdownSystem();
    LinearStrategyModule<LevelLoad>::ShutdownSystem();
    LinearStrategyModule<GlobalLoad>::ShutdownSystem();

    PoolModule<BucketScope<16>>::ShutdownSystem();
    PoolModule<BucketScope<32>>::ShutdownSystem();
    PoolModule<BucketScope<64>>::ShutdownSystem();
    PoolModule<BucketScope<128>>::ShutdownSystem();
    PoolModule<BucketScope<256>>::ShutdownSystem();

    m_HandleTable.Clear();
}

template <typename TScope> void AllocatorEngine::PrintStats(const char* ScopeName) const noexcept
{
    ContextStats::Snapshot Snap;

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
              << "  Allocated : " << FormatBytes(Snap.Allocated) << "\n"
              << "  Current   : " << FormatBytes(Snap.Current) << "\n"
              << "  Peak      : " << FormatBytes(Snap.Peak) << "\n"
              << "  Count     : " << Snap.Count << "\n";
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
