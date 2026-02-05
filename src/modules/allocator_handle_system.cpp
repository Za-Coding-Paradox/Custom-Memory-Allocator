#include "modules/allocator_handle_system.h"

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
    : m_FreeListHead(FREE_LIST_END), m_Capacity(0), m_ActiveCount(0) {

  for (auto& page : m_Pages) {
    page.store(nullptr, std::memory_order_relaxed);
  }

  if (InitialCapacity == 0)
    InitialCapacity = ELEMENTS_PER_PAGE;

  uint32_t pagesNeeded = (InitialCapacity + ELEMENTS_PER_PAGE - 1) / ELEMENTS_PER_PAGE;
  if (pagesNeeded == 0)
    pagesNeeded = 1;

  LOG_ALLOCATOR("INFO", "HandleTable: Initializing with " << pagesNeeded << " pages.");

  for (uint32_t i = 0; i < pagesNeeded; ++i) {
    GrowCapacity();
  }
}

HandleTable::~HandleTable() noexcept {
  LOG_ALLOCATOR("INFO", "HandleTable: Shutting down. Cleaning up pages.");
  for (auto& pageAtom : m_Pages) {
    HandleMetadata* page = pageAtom.load(std::memory_order_relaxed);
    if (page) {
      delete[] page;
      pageAtom.store(nullptr, std::memory_order_relaxed);
    }
  }
}

bool HandleTable::GrowCapacity() noexcept {
  std::lock_guard<std::mutex> lock(m_GrowthMutex);

  uint32_t currentCap = m_Capacity.load(std::memory_order_relaxed);
  uint32_t nextPageIndex = currentCap / ELEMENTS_PER_PAGE;

  if (nextPageIndex >= MAX_PAGES) {
    LOG_ALLOCATOR("CRITICAL",
                  "HandleTable: Max Capacity Reached (" << MAX_CAPACITY << "). Cannot Grow.");
    return false;
  }

  HandleMetadata* newPage = new (std::nothrow) HandleMetadata[ELEMENTS_PER_PAGE];
  if (!newPage) {
    LOG_ALLOCATOR("CRITICAL", "HandleTable: Failed to allocate new page (OOM).");
    return false;
  }

  uint32_t baseIndex = nextPageIndex * ELEMENTS_PER_PAGE;

  for (uint32_t i = 0; i < ELEMENTS_PER_PAGE - 1; ++i) {
    newPage[i].NextFree = baseIndex + i + 1;
    newPage[i].Generation = 1;
  }

  newPage[ELEMENTS_PER_PAGE - 1].NextFree = m_FreeListHead.load(std::memory_order_relaxed);
  newPage[ELEMENTS_PER_PAGE - 1].Generation = 1;

  m_Pages[nextPageIndex].store(newPage, std::memory_order_release);

  m_FreeListHead.store(baseIndex, std::memory_order_release);

  uint32_t newTotalCapacity = baseIndex + ELEMENTS_PER_PAGE;
  m_Capacity.store(newTotalCapacity, std::memory_order_release);

  LOG_ALLOCATOR("DEBUG", "HandleTable: Grew Capacity to " << newTotalCapacity << " (Page "
                                                          << nextPageIndex << ")");
  return true;
}

Handle HandleTable::Allocate(void* Pointer) noexcept {
  if (Pointer == nullptr) {
    LOG_ALLOCATOR("WARN", "HandleTable: Attempted to allocate handle for nullptr");
    return INVALID_HANDLE;
  }

  while (true) {
    uint32_t currentIndex = m_FreeListHead.load(std::memory_order_acquire);

    if (currentIndex == FREE_LIST_END) {
      if (!GrowCapacity()) {
        return INVALID_HANDLE;
      }
      continue;
    }

    // Calculate Page and Slot
    uint32_t pageIndex = currentIndex >> PAGE_SHIFT;
    uint32_t slotIndex = currentIndex & PAGE_MASK;

    HandleMetadata* page = m_Pages[pageIndex].load(std::memory_order_acquire);

    if (!page) [[unlikely]] {
      LOG_ALLOCATOR("CRITICAL",
                    "HandleTable: Null page encountered for valid index " << currentIndex);
      return INVALID_HANDLE;
    }

    uint32_t nextFree = page[slotIndex].NextFree;

    if (m_FreeListHead.compare_exchange_weak(currentIndex, nextFree, std::memory_order_release,
                                             std::memory_order_acquire)) {

      HandleMetadata& meta = page[slotIndex];
      meta.Pointer = Pointer;

      uint32_t generation = meta.Generation;

      m_ActiveCount.fetch_add(1, std::memory_order_relaxed);

      LOG_ALLOCATOR("DEBUG",
                    "HandleTable: Allocated Handle ID: " << currentIndex << " Gen: " << generation);
      return Handle(currentIndex, generation);
    }
  }
}

void* HandleTable::Resolve(Handle H) const noexcept {
  if (!H.IsValid()) {
    return nullptr;
  }

  uint32_t index = H.GetIndex();
  uint32_t generation = H.GetGeneration();

  if (index >= m_Capacity.load(std::memory_order_acquire)) {
    LOG_ALLOCATOR("WARN", "HandleTable: Resolve Index out of bounds: " << index);
    return nullptr;
  }

  uint32_t pageIndex = index >> PAGE_SHIFT;
  uint32_t slotIndex = index & PAGE_MASK;

  HandleMetadata* page = m_Pages[pageIndex].load(std::memory_order_acquire);

  if (!page) [[unlikely]] {
    return nullptr;
  }

  const HandleMetadata& meta = page[slotIndex];

  if (meta.Generation != generation) {
    LOG_ALLOCATOR("DEBUG", "HandleTable: Stale Handle Resolve (Gen Mismatch). ID: " << index);
    return nullptr;
  }

  return meta.Pointer;
}

bool HandleTable::Free(Handle H) noexcept {
  if (!H.IsValid()) {
    return false;
  }

  uint32_t index = H.GetIndex();
  uint32_t generation = H.GetGeneration();

  if (index >= m_Capacity.load(std::memory_order_acquire)) {
    LOG_ALLOCATOR("ERROR", "HandleTable: Free Index out of bounds: " << index);
    return false;
  }

  uint32_t pageIndex = index >> PAGE_SHIFT;
  uint32_t slotIndex = index & PAGE_MASK;

  HandleMetadata* page = m_Pages[pageIndex].load(std::memory_order_acquire);
  HandleMetadata& meta = page[slotIndex];

  if (meta.Generation != generation) {
    LOG_ALLOCATOR("WARN", "HandleTable: Double Free or Stale Handle detected. ID: " << index);
    return false;
  }

  meta.Pointer = nullptr;

  meta.Generation++;
  if (meta.Generation == 0)
    meta.Generation = 1;
  while (true) {
    uint32_t currentHead = m_FreeListHead.load(std::memory_order_acquire);
    meta.NextFree = currentHead;

    if (m_FreeListHead.compare_exchange_weak(currentHead, index, std::memory_order_release,
                                             std::memory_order_acquire)) {
      m_ActiveCount.fetch_sub(1, std::memory_order_relaxed);
      LOG_ALLOCATOR("DEBUG", "HandleTable: Freed Handle ID: " << index);
      return true;
    }
  }
}

bool HandleTable::IsValid(Handle H) const noexcept {
  if (!H.IsValid())
    return false;

  uint32_t index = H.GetIndex();
  if (index >= m_Capacity.load(std::memory_order_acquire))
    return false;

  uint32_t pageIndex = index >> PAGE_SHIFT;
  uint32_t slotIndex = index & PAGE_MASK;

  HandleMetadata* page = m_Pages[pageIndex].load(std::memory_order_acquire);
  return page && (page[slotIndex].Generation == H.GetGeneration());
}

bool HandleTable::Update(Handle H, void* NewPointer) noexcept {
  if (!IsValid(H))
    return false;

  uint32_t index = H.GetIndex();
  uint32_t pageIndex = index >> PAGE_SHIFT;
  uint32_t slotIndex = index & PAGE_MASK;

  HandleMetadata* page = m_Pages[pageIndex].load(std::memory_order_acquire);

  HandleMetadata& meta = page[slotIndex];
  if (meta.Generation == H.GetGeneration()) {
    meta.Pointer = NewPointer;
    return true;
  }
  return false;
}

void HandleTable::Clear() noexcept {
  LOG_ALLOCATOR("INFO", "HandleTable: Clearing all handles (Soft Reset).");

  uint32_t cap = m_Capacity.load(std::memory_order_relaxed);
  uint32_t pages = (cap + ELEMENTS_PER_PAGE - 1) / ELEMENTS_PER_PAGE;

  for (uint32_t p = 0; p < pages; ++p) {
    HandleMetadata* page = m_Pages[p].load(std::memory_order_relaxed);
    if (!page)
      continue;

    uint32_t baseIndex = p * ELEMENTS_PER_PAGE;
    for (uint32_t i = 0; i < ELEMENTS_PER_PAGE; ++i) {
      page[i].Pointer = nullptr;
      page[i].Generation++;
      if (page[i].Generation == 0)
        page[i].Generation = 1;

      page[i].NextFree = baseIndex + i + 1;
    }
  }

  if (pages > 0) {
    HandleMetadata* lastPage = m_Pages[pages - 1].load(std::memory_order_relaxed);
    lastPage[ELEMENTS_PER_PAGE - 1].NextFree = FREE_LIST_END;
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
  uint32_t cap = m_Capacity.load(std::memory_order_relaxed);
  return cap > 0 ? (static_cast<float>(m_ActiveCount.load(std::memory_order_relaxed)) / cap) : 0.0f;
}

} // namespace Allocator

namespace std {
size_t hash<Allocator::Handle>::operator()(const Allocator::Handle& H) const noexcept {
  return Allocator::Handle::Hash{}(H);
}
} // namespace std
