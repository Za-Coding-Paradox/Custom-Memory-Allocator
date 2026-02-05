#include <modules/allocator_engine.h>

namespace Allocator {

namespace {
constexpr size_t g_DefaultHandleCapacity = 1024;
constexpr size_t g_BytesPerKB = 1024;
constexpr size_t g_BytesPerMB = 1024 * 1024;
} // namespace

AllocatorEngine::AllocatorEngine(size_t SlabSize, size_t ArenaSize)
    : m_Registry(SlabSize, ArenaSize), m_HandleTable(g_DefaultHandleCapacity) {}

AllocatorEngine::~AllocatorEngine() { Shutdown(); }

void AllocatorEngine::Initialize() {
  LinearStrategyModule<FrameLoad>::InitializeModule(&m_Registry);
  LinearStrategyModule<LevelLoad>::InitializeModule(&m_Registry);
  LinearStrategyModule<GlobalLoad>::InitializeModule(&m_Registry);
}

void AllocatorEngine::Shutdown() {
  LinearStrategyModule<FrameLoad>::FlushThreadStats();
  LinearStrategyModule<LevelLoad>::FlushThreadStats();
  LinearStrategyModule<GlobalLoad>::FlushThreadStats();

  LinearStrategyModule<FrameLoad>::ShutdownModule();
  LinearStrategyModule<LevelLoad>::ShutdownModule();
  LinearStrategyModule<GlobalLoad>::ShutdownModule();

  LinearStrategyModule<FrameLoad>::ShutdownSystem();
  LinearStrategyModule<LevelLoad>::ShutdownSystem();
  LinearStrategyModule<GlobalLoad>::ShutdownSystem();

  m_HandleTable.Clear();
}

void AllocatorEngine::GenerateFullReport() const noexcept {
  std::cout << "================================================================\n";
  std::cout << "                   MEMORY ALLOCATION REPORT                     \n";
  std::cout << "================================================================\n";

  PrintStats<FrameLoad>("FrameLoad");
  PrintStats<LevelLoad>("LevelLoad");
  PrintStats<GlobalLoad>("GlobalLoad");

  std::cout << "\n[System Overview]\n";
  std::cout << "  Handle Capacity: " << m_HandleTable.GetCapacity() << "\n";
  std::cout << "  Active Handles : " << m_HandleTable.GetActiveCount() << "\n";
  std::cout << "================================================================\n";
}

void AllocatorEngine::ReportError(const char* Msg, std::source_location Loc) const noexcept {
  std::cerr << "[Allocator ERROR] " << Msg << "\n"
            << "  File: " << Loc.file_name() << ":" << Loc.line() << "\n"
            << "  Func: " << Loc.function_name() << "\n";
}

std::string AllocatorEngine::FormatBytes(size_t Bytes) noexcept {
  std::stringstream StringStream;
  if (Bytes < g_BytesPerKB) {
    StringStream << Bytes << " B";
  } else if (Bytes < g_BytesPerMB) {
    StringStream << std::fixed << std::setprecision(2)
                 << (static_cast<double>(Bytes) / static_cast<double>(g_BytesPerKB)) << " KB";
  } else {
    StringStream << std::fixed << std::setprecision(2)
                 << (static_cast<double>(Bytes) / static_cast<double>(g_BytesPerMB)) << " MB";
  }
  return StringStream.str();
}

} // namespace Allocator
