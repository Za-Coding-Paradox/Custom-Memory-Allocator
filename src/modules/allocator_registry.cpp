#include "modules/allocator_registry.h"

namespace Allocator {

SlabDescriptor::SlabDescriptor(const SlabConfig& Config) noexcept
    : m_SlabStart(Config.p_StartAddress), m_FreeListHead(Config.p_FreeListHead), m_ActiveSlots(0),
      m_TotalSlots(Config.p_TotalSlots), m_NextSlab(nullptr),
      m_AvailableSlabMemory(Config.p_SlabMemory) {}

void SlabDescriptor::ResetSlab() noexcept {
  m_FreeListHead = m_SlabStart;
  m_ActiveSlots = 0;
  m_NextSlab = nullptr;
}

void SlabDescriptor::SetSlabStart(uintptr_t StartAddress) noexcept { m_SlabStart = StartAddress; }

void SlabDescriptor::SetTotalSlots(size_t TotalSlots) noexcept { m_TotalSlots = TotalSlots; }

void SlabDescriptor::SetActiveSlots(size_t ActiveSlots) noexcept { m_ActiveSlots = ActiveSlots; }

void SlabDescriptor::IncrementActiveSlots() noexcept { m_ActiveSlots++; }

void SlabDescriptor::SetNextSlab(SlabDescriptor* NextSlab) noexcept { m_NextSlab = NextSlab; }

void SlabDescriptor::UpdateFreeListHead(uintptr_t FreeListHead) noexcept {
  m_FreeListHead = FreeListHead;
}

[[nodiscard]] uintptr_t SlabDescriptor::GetSlabStart() const noexcept { return m_SlabStart; }

[[nodiscard]] uintptr_t SlabDescriptor::GetFreeListHead() const noexcept { return m_FreeListHead; }

[[nodiscard]] size_t SlabDescriptor::GetTotalSlots() const noexcept { return m_TotalSlots; }

[[nodiscard]] size_t SlabDescriptor::GetActiveSlots() const noexcept { return m_ActiveSlots; }

[[nodiscard]] SlabDescriptor* SlabDescriptor::GetNextSlab() const noexcept { return m_NextSlab; }

[[nodiscard]] size_t SlabDescriptor::GetAvailableMemorySize() const noexcept {
  return m_AvailableSlabMemory;
}

SlabRegistry::SlabRegistry(size_t SlabSize, size_t RequestedArenaSize) noexcept
    : m_DescriptorCount(0), m_ArenaRegistryStart(nullptr), m_ArenaSlabsStart(nullptr),
      m_ArenaSize(RequestedArenaSize), m_SlabSize(SlabSize) {

  LOG_ALLOCATOR("INFO", "SlabRegistry: Registry Initialization Started.");

  if (!InitializeArena()) {
    LOG_ALLOCATOR("CRITICAL", "SlabRegistry: Registry failed to allocate OS Memory Arena.");
    exit(1);
  }

  m_DescriptorCount = m_ArenaSize / m_SlabSize;
  LOG_ALLOCATOR("INFO", "SlabRegistry: Descriptor Count Calculated: " << m_DescriptorCount);

  m_DescriptorSpan = std::span<SlabDescriptor>(static_cast<SlabDescriptor*>(m_ArenaRegistryStart),
                                               m_DescriptorCount);

  const size_t Uint64Count = (m_DescriptorCount + g_AlignmentMask) / g_BitsPerBlock;

  void* const BitMapStart =
      Utility::Add(m_ArenaRegistryStart, m_DescriptorCount * sizeof(SlabDescriptor));

  m_BitMap = std::span<uint64_t>(static_cast<uint64_t*>(BitMapStart), Uint64Count);

  std::ranges::fill(m_BitMap, g_EmptyBlock);

  void* const EndOfMetadata = Utility::Add(BitMapStart, Uint64Count * sizeof(uint64_t));
  m_ArenaSlabsStart = Utility::AlignForward(EndOfMetadata, m_SlabSize);

  for (const size_t SlabIndex : std::views::iota(0ULL, m_DescriptorCount)) {
    void* const PhysicalAddr = Utility::GetSlabStart(SlabIndex, m_ArenaSlabsStart, m_SlabSize);

    SlabDescriptor& Current = m_DescriptorSpan[SlabIndex];

    new (&Current)
        SlabDescriptor(SlabConfig{.p_StartAddress = std::bit_cast<uintptr_t>(PhysicalAddr),
                                  .p_FreeListHead = std::bit_cast<uintptr_t>(PhysicalAddr),
                                  .p_TotalSlots = 0,
                                  .p_SlabMemory = m_SlabSize});
  }

  LOG_ALLOCATOR("INFO", "SlabRegistry: Registry Initialization Complete.");
}

SlabRegistry::~SlabRegistry() noexcept {
  LOG_ALLOCATOR("INFO", "SlabRegistry: Registry Shutdown Initiated.");
  ShutdownArena();
}

bool SlabRegistry::InitializeArena() noexcept {
  void* RawMemory = nullptr;
#ifdef _WIN32
  RawMemory = VirtualAlloc(nullptr, m_ArenaSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (RawMemory == nullptr) {
    LOG_ALLOCATOR("ERROR", "SlabRegistry: VirtualAlloc failed - Error Code: " << GetLastError());
    return false;
  }
#else
  RawMemory =
      mmap(nullptr, m_ArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (RawMemory == MAP_FAILED) {
    LOG_ALLOCATOR("ERROR", "SlabRegistry: mmap failed during initialization.");
    return false;
  }
#endif
  m_ArenaRegistryStart = RawMemory;
  return true;
}

void SlabRegistry::ShutdownArena() noexcept {
  if (m_ArenaRegistryStart == nullptr) {
    return;
  }

#ifdef _WIN32
  if (!VirtualFree(m_ArenaRegistryStart, 0, MEM_RELEASE)) {
    LOG_ALLOCATOR("ERROR", "SlabRegistry: VirtualFree failed during shutdown.");
  }
#else
  munmap(m_ArenaRegistryStart, m_ArenaSize);
#endif
  m_ArenaRegistryStart = nullptr;
  m_ArenaSlabsStart = nullptr;
  m_DescriptorSpan = {};
  m_DescriptorCount = 0;
}

[[nodiscard]] size_t SlabRegistry::GetDescriptorCount() const noexcept { return m_DescriptorCount; }

[[nodiscard]] void* SlabRegistry::GetArenaSlabsStart() const noexcept { return m_ArenaSlabsStart; }

[[nodiscard]] size_t SlabRegistry::GetSlabSize() const noexcept { return m_SlabSize; }

[[nodiscard]] size_t SlabRegistry::GetArenaSize() const noexcept { return m_ArenaSize; }

[[nodiscard]] SlabDescriptor* SlabRegistry::AllocateSlab() noexcept {
  std::lock_guard<std::mutex> lock(m_AllocationMutex);

  for (size_t BlockIdx = 0; BlockIdx < m_BitMap.size(); ++BlockIdx) {
    uint64_t& CurrentBlock = m_BitMap[BlockIdx];

    if (CurrentBlock != g_FullBlock) [[likely]] {
      const int BitPos = std::countr_one(CurrentBlock);

      if (BlockIdx > m_DescriptorCount / g_BitsPerBlock) {
        LOG_ALLOCATOR("ERROR", "SlabRegistry: BlockIdx out of range.");
        return nullptr;
      }

      const size_t SlabIndex = (BlockIdx * g_BitsPerBlock) + static_cast<size_t>(BitPos);

      if (SlabIndex >= m_DescriptorCount) [[unlikely]] {
        LOG_ALLOCATOR("ERROR", "SlabRegistry: AllocateSlab OOM - Index out of bounds.");
        return nullptr;
      }

      CurrentBlock |= (1ULL << BitPos);

      SlabDescriptor& Descriptor = m_DescriptorSpan[SlabIndex];
      Descriptor.ResetSlab();

      LOG_ALLOCATOR("DEBUG", "SlabRegistry: Slab Allocated - Index: " << SlabIndex);
      return &Descriptor;
    }
  }

  LOG_ALLOCATOR("CRITICAL", "SlabRegistry: AllocateSlab Failed - Arena Full.");
  return nullptr;
}

void SlabRegistry::FreeSlab(SlabDescriptor* SlabToFree) noexcept {
  std::lock_guard<std::mutex> lock(m_AllocationMutex);

  if (SlabToFree == nullptr) {
    LOG_ALLOCATOR("WARN", "SlabRegistry: Attempted to free nullptr slab.");
    return;
  }

  const size_t SlabIndex = Utility::GetSlabIndex(std::bit_cast<void*>(SlabToFree->GetSlabStart()),
                                                 m_ArenaSlabsStart, m_SlabSize);

  if (SlabIndex >= m_DescriptorCount) [[unlikely]] {
    LOG_ALLOCATOR("ERROR", "SlabRegistry: FreeSlab failed - Invalid Slab Index: " << SlabIndex);
    return;
  }

  const size_t BlockIdx = SlabIndex / g_BitsPerBlock;
  const size_t BitIdx = SlabIndex % g_BitsPerBlock;

  m_BitMap[BlockIdx] &= ~(1ULL << BitIdx);

  SlabToFree->ResetSlab();
  LOG_ALLOCATOR("DEBUG", "SlabRegistry: Slab Freed - Index: " << SlabIndex);
}
} // namespace Allocator
