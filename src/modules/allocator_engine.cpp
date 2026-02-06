#include <iomanip>
#include <iostream>
#include <modules/allocator_engine.h>
#include <sstream>

namespace Allocator {

namespace {
constexpr size_t g_DefaultHandleCapacity = 1024;
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

    std::cout << "\n[" << ScopeName << " Stats]\n";
    std::cout << "  Allocated : " << FormatBytes(Snap.Allocated) << "\n";
    std::cout << "  Current   : " << FormatBytes(Snap.Current) << "\n";
    std::cout << "  Peak      : " << FormatBytes(Snap.Peak) << "\n";
    std::cout << "  Count     : " << Snap.Count << "\n";
}

void AllocatorEngine::GenerateFullReport() const noexcept
{
    std::cout << "================================================================\n";
    std::cout << "                   MEMORY ALLOCATION REPORT                     \n";
    std::cout << "================================================================\n";

    PrintStats<FrameLoad>("Linear: FrameLoad");
    PrintStats<LevelLoad>("Linear: LevelLoad");
    PrintStats<GlobalLoad>("Linear: GlobalLoad");

    std::cout << "\n--- Shared Pool Buckets ---";
    PrintStats<BucketScope<16>>("Pool: 16B");
    PrintStats<BucketScope<32>>("Pool: 32B");
    PrintStats<BucketScope<64>>("Pool: 64B");
    PrintStats<BucketScope<128>>("Pool: 128B");
    PrintStats<BucketScope<256>>("Pool: 256B");

    std::cout << "\n[System Overview]\n";
    std::cout << "  Handle Capacity: " << m_HandleTable.GetCapacity() << "\n";
    std::cout << "  Active Handles : " << m_HandleTable.GetActiveCount() << "\n";
    std::cout << "================================================================\n";
}

void AllocatorEngine::ReportError(const char* Msg, std::source_location Loc) const noexcept
{
    std::cerr << "[Allocator ERROR] " << Msg << "\n"
              << "  File: " << Loc.file_name() << ":" << Loc.line() << "\n"
              << "  Func: " << Loc.function_name() << "\n";
}

std::string AllocatorEngine::FormatBytes(size_t Bytes) noexcept
{
    std::stringstream StringStream;
    if (Bytes < g_BytesPerKB) {
        StringStream << Bytes << " B";
    }
    else if (Bytes < g_BytesPerMB) {
        StringStream << std::fixed << std::setprecision(2)
                     << (static_cast<double>(Bytes) / static_cast<double>(g_BytesPerKB)) << " KB";
    }
    else {
        StringStream << std::fixed << std::setprecision(2)
                     << (static_cast<double>(Bytes) / static_cast<double>(g_BytesPerMB)) << " MB";
    }
    return StringStream.str();
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
