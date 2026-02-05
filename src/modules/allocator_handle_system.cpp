#include <modules/allocator_handle_system.h>
#include <new> // For std::nothrow
#include <utilities/allocator_utility.h>

namespace Allocator {

uint32_t Handle::GetIndex() const noexcept { return static_cast<uint32_t>(m_Packed & 0xFFFFFFFF); }

uint32_t Handle::GetGeneration() const noexcept { return static_cast<uint32_t>(m_Packed >> 32); }

uint64_t Handle::GetPacked() const noexcept { return m_Packed; }

bool Handle::IsValid() const noexcept { return m_Packed != 0; }

bool Handle::operator==(const Handle& Other) const noexcept { return m_Packed == Other.m_Packed; }

bool Handle::operator!=(const Handle& Other) const noexcept { return m_Packed != Other.m_Packed; }

bool Handle::operator<(const Handle& Other) const noexcept { return m_Packed < Other.m_Packed; }

size_t Handle::Hash::operator()(const Handle& H) const noexcept {
  return std::hash<uint64_t>{}(H.m_Packed);
}

HandleMetadata::HandleMetadata() noexcept : Pointer(nullptr), Generation(1), NextFree(0) {}

HandleTable::HandleTable(uint32_t InitialCapacity) noexcept
    : m_FreeListHead(g_FreeListEnd), m_Capacity(0), m_ActiveCount(0) {

  for (auto& Page : m_Pages) {
    Page.store(nullptr, std::memory_order_relaxed);
  }

  if (InitialCapacity == 0) {
    InitialCapacity = g_ElementsPerPage;
  }

  uint32_t PagesNeeded = (InitialCapacity + g_ElementsPerPage - 1) / g_ElementsPerPage;
  if (PagesNeeded == 0) {
    PagesNeeded = 1;
  }

  LOG_ALLOCATOR("INFO", "HandleTable: Initializing with " << PagesNeeded << " pages.");

  for (uint32_t i = 0; i < PagesNeeded; ++i) {
    GrowCapacity();
  }
}

HandleTable::~HandleTable() noexcept {
  LOG_ALLOCATOR("INFO", "HandleTable: Shutting down. Cleaning up pages.");
  for (auto& PageAtom : m_Pages) {
    HandleMetadata* Page = PageAtom.load(std::memory_order_relaxed);
    if (Page != nullptr) {
      delete[] Page;
      PageAtom.store(nullptr, std::memory_order_relaxed);
    }
  }
}

bool HandleTable::GrowCapacity() noexcept {
  std::lock_guard<std::mutex> Lock(m_GrowthMutex);

  uint32_t CurrentCap = m_Capacity.load(std::memory_order_relaxed);
  uint32_t NextPageIndex = CurrentCap / g_ElementsPerPage;

  if (NextPageIndex >= g_MaxPages) {
    LOG_ALLOCATOR("CRITICAL",
                  "HandleTable: Max Capacity Reached (" << g_MaxCapacity << "). Cannot Grow.");
    return false;
  }

  HandleMetadata* NewPage = new (std::nothrow) HandleMetadata[g_ElementsPerPage];
  if (NewPage == nullptr) {
    LOG_ALLOCATOR("CRITICAL", "HandleTable: Failed to allocate new page (OOM).");
    return false;
  }

  uint32_t BaseIndex = NextPageIndex * g_ElementsPerPage;

  for (uint32_t i = 0; i < g_ElementsPerPage - 1; ++i) {
    NewPage[i].NextFree = BaseIndex + i + 1;
    NewPage[i].Generation = 1;
  }

  NewPage[g_ElementsPerPage - 1].NextFree = m_FreeListHead.load(std::memory_order_relaxed);
  NewPage[g_ElementsPerPage - 1].Generation = 1;

  m_Pages[NextPageIndex].store(NewPage, std::memory_order_release);

  m_FreeListHead.store(BaseIndex, std::memory_order_release);

  uint32_t NewTotalCapacity = BaseIndex + g_ElementsPerPage;
  m_Capacity.store(NewTotalCapacity, std::memory_order_release);

  LOG_ALLOCATOR("DEBUG", "HandleTable: Grew Capacity to " << NewTotalCapacity << " (Page "
                                                          << NextPageIndex << ")");
  return true;
}

Handle HandleTable::Allocate(void* Pointer) noexcept {
  if (Pointer == nullptr) {
    LOG_ALLOCATOR("WARN", "HandleTable: Attempted to allocate handle for nullptr");
    return g_InvalidHandle;
  }

  while (true) {
    uint32_t CurrentIndex = m_FreeListHead.load(std::memory_order_acquire);

    if (CurrentIndex == g_FreeListEnd) {
      if (!GrowCapacity()) {
        return g_InvalidHandle;
      }
      continue;
    }

    uint32_t PageIndex = CurrentIndex >> g_PageShift;
    uint32_t SlotIndex = CurrentIndex & g_PageMask;

    HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);

    if (Page == nullptr) [[unlikely]] {
      LOG_ALLOCATOR("CRITICAL",
                    "HandleTable: Null page encountered for valid index " << CurrentIndex);
      return g_InvalidHandle;
    }

    uint32_t NextFree = Page[SlotIndex].NextFree;

    if (m_FreeListHead.compare_exchange_weak(CurrentIndex, NextFree, std::memory_order_release,
                                             std::memory_order_acquire)) {

      HandleMetadata& Meta = Page[SlotIndex];
      Meta.Pointer = Pointer;

      uint32_t Generation = Meta.Generation;

      m_ActiveCount.fetch_add(1, std::memory_order_relaxed);

      LOG_ALLOCATOR("DEBUG",
                    "HandleTable: Allocated Handle ID: " << CurrentIndex << " Gen: " << Generation);
      return Handle(CurrentIndex, Generation);
    }
  }
}

void* HandleTable::Resolve(Handle H) const noexcept {
  if (!H.IsValid()) {
    return nullptr;
  }

  uint32_t Index = H.GetIndex();
  uint32_t Generation = H.GetGeneration();

  if (Index >= m_Capacity.load(std::memory_order_acquire)) {
    LOG_ALLOCATOR("WARN", "HandleTable: Resolve Index out of bounds: " << Index);
    return nullptr;
  }

  uint32_t PageIndex = Index >> g_PageShift;
  uint32_t SlotIndex = Index & g_PageMask;

  HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);

  if (Page == nullptr) [[unlikely]] {
    return nullptr;
  }

  const HandleMetadata& Meta = Page[SlotIndex];

  if (Meta.Generation != Generation) {
    LOG_ALLOCATOR("DEBUG", "HandleTable: Stale Handle Resolve (Gen Mismatch). ID: " << Index);
    return nullptr;
  }

  return Meta.Pointer;
}

bool HandleTable::Free(Handle H) noexcept {
  if (!H.IsValid()) {
    return false;
  }

  uint32_t Index = H.GetIndex();
  uint32_t Generation = H.GetGeneration();

  if (Index >= m_Capacity.load(std::memory_order_acquire)) {
    LOG_ALLOCATOR("ERROR", "HandleTable: Free Index out of bounds: " << Index);
    return false;
  }

  uint32_t PageIndex = Index >> g_PageShift;
  uint32_t SlotIndex = Index & g_PageMask;

  HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);
  HandleMetadata& Meta = Page[SlotIndex];

  if (Meta.Generation != Generation) {
    LOG_ALLOCATOR("WARN", "HandleTable: Double Free or Stale Handle detected. ID: " << Index);
    return false;
  }

  Meta.Pointer = nullptr;

  Meta.Generation++;
  if (Meta.Generation == 0) {
    Meta.Generation = 1;
  }

  while (true) {
    uint32_t CurrentHead = m_FreeListHead.load(std::memory_order_acquire);
    Meta.NextFree = CurrentHead;

    if (m_FreeListHead.compare_exchange_weak(CurrentHead, Index, std::memory_order_release,
                                             std::memory_order_acquire)) {
      m_ActiveCount.fetch_sub(1, std::memory_order_relaxed);
      LOG_ALLOCATOR("DEBUG", "HandleTable: Freed Handle ID: " << Index);
      return true;
    }
  }
}

bool HandleTable::IsValid(Handle H) const noexcept {
  if (!H.IsValid()) {
    return false;
  }

  uint32_t Index = H.GetIndex();
  if (Index >= m_Capacity.load(std::memory_order_acquire)) {
    return false;
  }

  uint32_t PageIndex = Index >> g_PageShift;
  uint32_t SlotIndex = Index & g_PageMask;

  HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);
  return (Page != nullptr) && (Page[SlotIndex].Generation == H.GetGeneration());
}

bool HandleTable::Update(Handle H, void* NewPointer) noexcept {
  if (!IsValid(H)) {
    return false;
  }

  uint32_t Index = H.GetIndex();
  uint32_t PageIndex = Index >> g_PageShift;
  uint32_t SlotIndex = Index & g_PageMask;

  HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);

  HandleMetadata& Meta = Page[SlotIndex];
  if (Meta.Generation == H.GetGeneration()) {
    Meta.Pointer = NewPointer;
    return true;
  }
  return false;
}

void HandleTable::Clear() noexcept {
  LOG_ALLOCATOR("INFO", "HandleTable: Clearing all handles (Soft Reset).");

  uint32_t Cap = m_Capacity.load(std::memory_order_relaxed);
  uint32_t Pages = (Cap + g_ElementsPerPage - 1) / g_ElementsPerPage;

  for (uint32_t p = 0; p < Pages; ++p) {
    HandleMetadata* Page = m_Pages[p].load(std::memory_order_relaxed);
    if (Page == nullptr) {
      continue;
    }

    uint32_t BaseIndex = p * g_ElementsPerPage;
    for (uint32_t i = 0; i < g_ElementsPerPage; ++i) {
      Page[i].Pointer = nullptr;
      Page[i].Generation++;
      if (Page[i].Generation == 0) {
        Page[i].Generation = 1;
      }

      Page[i].NextFree = BaseIndex + i + 1;
    }
  }

  if (Pages > 0) {
    HandleMetadata* LastPage = m_Pages[Pages - 1].load(std::memory_order_relaxed);
    LastPage[g_ElementsPerPage - 1].NextFree = g_FreeListEnd;
  }

  m_FreeListHead.store(0, std::memory_order_release);
  m_ActiveCount.store(0, std::memory_order_release);
}

uint32_t HandleTable::GetActiveCount() const noexcept {
  return m_ActiveCount.load(std::memory_order_relaxed);
}

uint32_t HandleTable::GetCapacity() const noexcept {
  return m_Capacity.load(std::memory_order_relaxed);
}

float HandleTable::GetUtilization() const noexcept {
  uint32_t Cap = m_Capacity.load(std::memory_order_relaxed);
  return Cap > 0 ? (static_cast<float>(m_ActiveCount.load(std::memory_order_relaxed)) /
                    static_cast<float>(Cap))
                 : 0.0f;
}

} // namespace Allocator

namespace std {
size_t hash<Allocator::Handle>::operator()(const Allocator::Handle& H) const noexcept {
  return Allocator::Handle::Hash{}(H);
}
} // namespace std
