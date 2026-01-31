#include "modules/allocator_engine.h"

namespace Allocator {

void AllocationStats::RecordAllocation(size_t Size) noexcept {
  AllocationCount.fetch_add(1, std::memory_order_relaxed);
  size_t current = TotalAllocated.fetch_add(Size, std::memory_order_relaxed) + Size;

  size_t peak = PeakAllocated.load(std::memory_order_relaxed);
  while (current > peak &&
         !PeakAllocated.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {
  }
}

void AllocationStats::RecordFree(size_t Size) noexcept {
  FreeCount.fetch_add(1, std::memory_order_relaxed);
  TotalAllocated.fetch_sub(Size, std::memory_order_relaxed);
}

void AllocationStats::RecordReset() noexcept {
  ResetCount.fetch_add(1, std::memory_order_relaxed);
  TotalAllocated.store(0, std::memory_order_relaxed);
}

void AllocationStats::Clear() noexcept {
  TotalAllocated.store(0, std::memory_order_relaxed);
  PeakAllocated.store(0, std::memory_order_relaxed);
  AllocationCount.store(0, std::memory_order_relaxed);
  FreeCount.store(0, std::memory_order_relaxed);
  ResetCount.store(0, std::memory_order_relaxed);
}

void AllocationStats::Publish(size_t LocalAllocated, size_t LocalPeak, size_t LocalCount) noexcept {
  TotalAllocated.fetch_add(LocalAllocated, std::memory_order_relaxed);
  AllocationCount.fetch_add(LocalCount, std::memory_order_relaxed);

  // Update Peak atomically
  size_t currentPeak = PeakAllocated.load(std::memory_order_relaxed);
  while (LocalPeak > currentPeak &&
         !PeakAllocated.compare_exchange_weak(currentPeak, LocalPeak, std::memory_order_relaxed)) {
  }
}

AllocatorEngine::AllocatorEngine(size_t SlabSize, size_t ArenaSize,
                                 uint32_t HandleCapacity) noexcept
    : m_Registry(SlabSize, ArenaSize), m_HandleTable(HandleCapacity), m_SlabSize(SlabSize),
      m_ArenaSize(ArenaSize) {

  m_ContextStats[AllocatorContextType::Frame];
  m_ContextStats[AllocatorContextType::Level];
  m_ContextStats[AllocatorContextType::Global];
  m_ContextStats[AllocatorContextType::Temporary];
  m_ContextStats[AllocatorContextType::Persistent];
}

AllocatorEngine::~AllocatorEngine() noexcept { Shutdown(); }

void AllocatorEngine::Initialize() noexcept {
  std::lock_guard<std::mutex> lock(m_InitMutex);

  if (m_IsInitialized.load(std::memory_order_acquire)) {
    return;
  }

  LOG_ALLOCATOR("INFO", "AllocatorEngine: Initializing...");

  LinearStrategyModule<FrameLoad>::InitializeModule(&m_Registry,
                                                    &m_ContextStats[AllocatorContextType::Frame]);
  LinearStrategyModule<LevelLoad>::InitializeModule(&m_Registry,
                                                    &m_ContextStats[AllocatorContextType::Level]);
  LinearStrategyModule<GlobalLoad>::InitializeModule(&m_Registry,
                                                     &m_ContextStats[AllocatorContextType::Global]);

  m_IsInitialized.store(true, std::memory_order_release);

  LOG_ALLOCATOR("INFO", "AllocatorEngine: Initialization complete");
  LOG_ALLOCATOR("INFO", "  - Slab Size: " << m_SlabSize << " bytes");
  LOG_ALLOCATOR("INFO", "  - Arena Size: " << m_ArenaSize << " bytes");
  LOG_ALLOCATOR("INFO", "  - Handle Capacity: " << m_HandleTable.GetCapacity());
}

void AllocatorEngine::Shutdown() noexcept {
  std::lock_guard<std::mutex> lock(m_InitMutex);

  if (!m_IsInitialized.load(std::memory_order_acquire)) {
    return;
  }

  LOG_ALLOCATOR("INFO", "AllocatorEngine: Shutting down...");

  PrintStatistics();

  LinearStrategyModule<FrameLoad>::ShutdownModule();
  LinearStrategyModule<LevelLoad>::ShutdownModule();
  LinearStrategyModule<GlobalLoad>::ShutdownModule();

  m_HandleTable.Clear();

  m_IsInitialized.store(false, std::memory_order_release);
  LOG_ALLOCATOR("INFO", "AllocatorEngine: Shutdown complete");
}

const AllocationStats& AllocatorEngine::GetStats(AllocatorContextType Context) const noexcept {
  return m_ContextStats.at(Context);
}

size_t AllocatorEngine::GetTotalAllocated() const noexcept {
  size_t total = 0;
  for (const auto& [context, stats] : m_ContextStats) {
    total += stats.TotalAllocated.load(std::memory_order_relaxed);
  }
  return total;
}

size_t AllocatorEngine::GetPeakAllocated() const noexcept {
  size_t peak = 0;
  for (const auto& [context, stats] : m_ContextStats) {
    peak = std::max(peak, stats.PeakAllocated.load(std::memory_order_relaxed));
  }
  return peak;
}

void AllocatorEngine::PrintStatistics() const noexcept {
  LOG_ALLOCATOR("INFO", "=== AllocatorEngine Statistics ===");

  for (const auto& [context, stats] : m_ContextStats) {
    const char* contextName = GetContextName(context);
    size_t allocated = stats.TotalAllocated.load(std::memory_order_relaxed);
    size_t peak = stats.PeakAllocated.load(std::memory_order_relaxed);
    size_t allocs = stats.AllocationCount.load(std::memory_order_relaxed);
    size_t frees = stats.FreeCount.load(std::memory_order_relaxed);
    size_t resets = stats.ResetCount.load(std::memory_order_relaxed);

    LOG_ALLOCATOR("INFO", "Context: " << contextName);
    LOG_ALLOCATOR("INFO", "  Current: " << allocated << " bytes");
    LOG_ALLOCATOR("INFO", "  Peak: " << peak << " bytes");
    LOG_ALLOCATOR("INFO", "  Allocations: " << allocs);
    LOG_ALLOCATOR("INFO", "  Frees: " << frees);
    LOG_ALLOCATOR("INFO", "  Resets: " << resets);
  }

  LOG_ALLOCATOR("INFO", "Handle Table:");
  LOG_ALLOCATOR("INFO", "  Active: " << m_HandleTable.GetActiveCount());
  LOG_ALLOCATOR("INFO", "  Capacity: " << m_HandleTable.GetCapacity());
  LOG_ALLOCATOR("INFO", "  Utilization: " << (m_HandleTable.GetUtilization() * 100.0f) << "%");
}

void AllocatorEngine::ClearStatistics() noexcept {
  for (auto& [context, stats] : m_ContextStats) {
    stats.Clear();
  }
}

SlabRegistry& AllocatorEngine::GetRegistry() noexcept { return m_Registry; }

const SlabRegistry& AllocatorEngine::GetRegistry() const noexcept { return m_Registry; }

HandleTable& AllocatorEngine::GetHandleTable() noexcept { return m_HandleTable; }

const HandleTable& AllocatorEngine::GetHandleTable() const noexcept { return m_HandleTable; }

bool AllocatorEngine::IsInitialized() const noexcept {
  return m_IsInitialized.load(std::memory_order_acquire);
}

const char* AllocatorEngine::GetContextName(AllocatorContextType Type) noexcept {
  switch (Type) {
  case AllocatorContextType::Frame:
    return "Frame";
  case AllocatorContextType::Level:
    return "Level";
  case AllocatorContextType::Global:
    return "Global";
  case AllocatorContextType::Temporary:
    return "Temporary";
  case AllocatorContextType::Persistent:
    return "Persistent";
  default:
    return "Unknown";
  }
}

} // namespace Allocator
