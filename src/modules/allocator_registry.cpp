#include <modules/allocator_registry.h>

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
    : m_DescriptorCount(0), m_BitMapSizeInWords(0), m_ArenaRegistryStart(nullptr),
      m_ArenaSlabsStart(nullptr), m_ArenaSize(RequestedArenaSize), m_SlabSize(SlabSize) {
  if (static_cast<bool>(InitializeArena()) == false) {
    LOG_ALLOCATOR("CRITICAL", "SlabRegistry: Failed to initialize arena.");
  }
}

SlabRegistry::~SlabRegistry() noexcept { ShutdownArena(); }

bool SlabRegistry::InitializeArena() noexcept {
  m_ArenaRegistryStart =
      mmap(nullptr, m_ArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m_ArenaRegistryStart == MAP_FAILED) {
    m_ArenaRegistryStart = nullptr;
    return false;
  }

  size_t UnitSize = sizeof(SlabDescriptor) + m_SlabSize;
  size_t EstimatedCount = m_ArenaSize / UnitSize;
  if (EstimatedCount * UnitSize + 4096 > m_ArenaSize) {
    EstimatedCount--;
  }

  m_DescriptorCount = EstimatedCount;
  if (m_DescriptorCount == 0) {
    return false;
  }

  SlabDescriptor* DescriptorBase = static_cast<SlabDescriptor*>(m_ArenaRegistryStart);
  uintptr_t BaseAddr = reinterpret_cast<uintptr_t>(m_ArenaRegistryStart);
  uintptr_t SlabRegionStart = BaseAddr + (m_DescriptorCount * sizeof(SlabDescriptor));

  const size_t Alignment = 4096;
  size_t Padding = 0;
  if ((SlabRegionStart % Alignment) != 0) {
    Padding = Alignment - (SlabRegionStart % Alignment);
  }
  SlabRegionStart += Padding;

  m_ArenaSlabsStart = std::bit_cast<void*>(SlabRegionStart);
  uint8_t* CurrentSlabPtr = static_cast<uint8_t*>(m_ArenaSlabsStart);

  m_BitMapSizeInWords = (m_DescriptorCount + 63) / 64;
  m_BitMap = std::make_unique<std::atomic<uint64_t>[]>(m_BitMapSizeInWords);
  for (size_t Index = 0; Index < m_BitMapSizeInWords; ++Index) {
    m_BitMap[Index].store(0, std::memory_order_relaxed);
  }

  for (size_t Index = 0; Index < m_DescriptorCount; ++Index) {
    if (reinterpret_cast<uintptr_t>(CurrentSlabPtr + m_SlabSize) > (BaseAddr + m_ArenaSize)) {
      m_DescriptorCount = Index;
      break;
    }

    SlabConfig Config;
    Config.p_StartAddress = reinterpret_cast<uintptr_t>(CurrentSlabPtr);
    Config.p_FreeListHead = Config.p_StartAddress;
    Config.p_SlabMemory = m_SlabSize;
    Config.p_TotalSlots = 0;

    new (&DescriptorBase[Index]) SlabDescriptor(Config);
    CurrentSlabPtr += m_SlabSize;
  }

  m_DescriptorSpan = std::span<SlabDescriptor>(DescriptorBase, m_DescriptorCount);
  return true;
}

void SlabRegistry::ShutdownArena() noexcept {
  if (static_cast<bool>(m_ArenaRegistryStart)) {
    munmap(m_ArenaRegistryStart, m_ArenaSize);
    m_ArenaRegistryStart = nullptr;
  }
}

[[nodiscard]] SlabDescriptor* SlabRegistry::AllocateSlab() noexcept {
  for (size_t Index = 0; Index < m_BitMapSizeInWords; ++Index) {
    uint64_t CurrentWord = m_BitMap[Index].load(std::memory_order_relaxed);
    if (CurrentWord == ~0ULL) {
      continue;
    }

    int BitIndex = std::countr_one(CurrentWord);
    if (BitIndex >= 64) {
      continue;
    }

    uint64_t Mask = static_cast<uint64_t>(1ULL) << BitIndex;

    if ((CurrentWord & Mask) == 0) {
      if (static_cast<bool>(m_BitMap[Index].compare_exchange_weak(CurrentWord, CurrentWord | Mask,
                                                                  std::memory_order_acquire,
                                                                  std::memory_order_relaxed))) {
        size_t SlabIndex = (Index * 64) + static_cast<size_t>(BitIndex);
        if (SlabIndex >= m_DescriptorCount) {
          return nullptr;
        }

        SlabDescriptor* Slab = std::launder(&m_DescriptorSpan[SlabIndex]);
        Slab->ResetSlab();
        return Slab;
      }
    }
  }
  return nullptr;
}

void SlabRegistry::FreeSlab(SlabDescriptor* SlabToFree) noexcept {
  if (static_cast<bool>(SlabToFree) == false) {
    return;
  }

  SlabDescriptor* Base = m_DescriptorSpan.data();
  SlabDescriptor* End = Base + m_DescriptorCount;
  if (SlabToFree < Base || SlabToFree >= End) {
    return;
  }

  ptrdiff_t PtrDiffIndex = SlabToFree - Base;
  size_t WordIndex = static_cast<size_t>(PtrDiffIndex) / 64;
  size_t BitIndex = static_cast<size_t>(PtrDiffIndex) % 64;
  uint64_t Mask = static_cast<uint64_t>(1ULL) << BitIndex;
  m_BitMap[WordIndex].fetch_and(~Mask, std::memory_order_release);
}

} // namespace Allocator
