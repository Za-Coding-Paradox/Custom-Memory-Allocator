#include "modules/allocator_handle_system.h" // Ensure this path matches your include structure
#include <algorithm>                         // For std::max, std::min

namespace Allocator {

// ============================================================================
// Handle Implementation
// ============================================================================
uint32_t Handle::GetIndex() const noexcept {
  return static_cast<uint32_t>(m_Packed & 0xFFFFFFFF);
}

uint32_t Handle::GetGeneration() const noexcept {
  return static_cast<uint32_t>(m_Packed >> 32);
}

uint64_t Handle::GetPacked() const noexcept { return m_Packed; }

bool Handle::IsValid() const noexcept { return m_Packed != 0; }

bool Handle::operator==(const Handle &Other) const noexcept {
  return m_Packed == Other.m_Packed;
}

bool Handle::operator!=(const Handle &Other) const noexcept {
  return m_Packed != Other.m_Packed;
}

bool Handle::operator<(const Handle &Other) const noexcept {
  return m_Packed < Other.m_Packed;
}

size_t Handle::Hash::operator()(const Handle &H) const noexcept {
  return std::hash<uint64_t>{}(H.m_Packed);
}

// ============================================================================
// HandleMetadata Implementation
// ============================================================================
HandleMetadata::HandleMetadata() noexcept
    : Pointer(nullptr), Generation(1), NextFree(0) {}

// ============================================================================
// HandleTable Implementation
// ============================================================================
HandleTable::HandleTable(uint32_t InitialCapacity) noexcept
    : m_FreeListHead(0), m_Capacity(0), m_ActiveCount(0) {

  // Clamp to valid range
  InitialCapacity =
      std::max(uint32_t(16), std::min(InitialCapacity, MAX_CAPACITY));

  // Reserve metadata storage
  m_Metadata.resize(InitialCapacity);

  // Initialize free list
  InitializeFreeList(0, InitialCapacity);

  m_Capacity.store(InitialCapacity, std::memory_order_release);

  LOG_ALLOCATOR("INFO",
                "HandleTable: Initialized with capacity " << InitialCapacity);
}

void HandleTable::InitializeFreeList(uint32_t Start, uint32_t End) noexcept {
  // Build linked list of free slots
  for (uint32_t i = Start; i < End - 1; ++i) {
    m_Metadata[i].NextFree = i + 1;
    m_Metadata[i].Pointer = nullptr;
    // Generation stays at current value (or 1 for new slots)
    if (m_Metadata[i].Generation == 0) {
      m_Metadata[i].Generation = 1;
    }
  }

  // Last slot points to current free list head or END marker
  if (End > 0) {
    uint32_t currentHead = m_FreeListHead.load(std::memory_order_relaxed);
    m_Metadata[End - 1].NextFree = (Start == 0) ? FREE_LIST_END : currentHead;
  }

  // Update free list head to start of new range
  if (End > Start) {
    m_FreeListHead.store(Start, std::memory_order_release);
  }
}

bool HandleTable::GrowCapacity() noexcept {
  std::lock_guard<std::mutex> lock(m_GrowthMutex);

  uint32_t currentCapacity = m_Capacity.load(std::memory_order_acquire);

  // Check if already grown by another thread
  if (m_FreeListHead.load(std::memory_order_acquire) != FREE_LIST_END) {
    return true; // Another thread already grew
  }

  // Calculate new capacity (double it, but cap at MAX_CAPACITY)
  uint32_t newCapacity = std::min(currentCapacity * 2, MAX_CAPACITY);

  if (newCapacity <= currentCapacity) {
    LOG_ALLOCATOR("ERROR", "HandleTable: Cannot grow beyond MAX_CAPACITY");
    return false;
  }

  // Resize metadata storage
  size_t oldSize = m_Metadata.size();
  m_Metadata.resize(newCapacity);

  // Initialize new free list entries
  InitializeFreeList(static_cast<uint32_t>(oldSize), newCapacity);

  // Update capacity
  m_Capacity.store(newCapacity, std::memory_order_release);

  LOG_ALLOCATOR("INFO", "HandleTable: Grew capacity from "
                            << currentCapacity << " to " << newCapacity);

  return true;
}

Handle HandleTable::Allocate(void *Pointer) noexcept {
  if (Pointer == nullptr) {
    LOG_ALLOCATOR("WARN",
                  "HandleTable: Attempted to allocate handle for nullptr");
    return INVALID_HANDLE;
  }

  // Lock-free allocation using CAS
  while (true) {
    uint32_t index = m_FreeListHead.load(std::memory_order_acquire);

    // Check if free list is empty
    if (index == FREE_LIST_END) {
      // Try to grow capacity
      if (!GrowCapacity()) {
        LOG_ALLOCATOR("ERROR",
                      "HandleTable: Allocation failed - out of capacity");
        return INVALID_HANDLE;
      }
      continue; // Retry after growth
    }

    // Validate index
    if (index >= m_Capacity.load(std::memory_order_acquire)) {
      LOG_ALLOCATOR("CRITICAL",
                    "HandleTable: Corrupted free list - invalid index "
                        << index);
      return INVALID_HANDLE;
    }

    // Get next free index
    uint32_t nextFree = m_Metadata[index].NextFree;

    // Try to atomically update free list head
    if (m_FreeListHead.compare_exchange_weak(index, nextFree,
                                             std::memory_order_release,
                                             std::memory_order_acquire)) {
      // Successfully claimed this slot
      HandleMetadata &meta = m_Metadata[index];

      // Update metadata
      meta.Pointer = Pointer;
      // Generation is already set (incremented on free)
      uint32_t generation = meta.Generation;

      // Increment active count
      m_ActiveCount.fetch_add(1, std::memory_order_relaxed);

      LOG_ALLOCATOR("DEBUG", "HandleTable: Allocated handle - Index: "
                                 << index << " Generation: " << generation);

      return Handle(index, generation);
    }

    // CAS failed, another thread took this slot - retry
  }
}

void *HandleTable::Resolve(Handle H) const noexcept {
  if (!H.IsValid()) {
    return nullptr;
  }

  uint32_t index = H.GetIndex();
  uint32_t generation = H.GetGeneration();

  // Bounds check
  if (index >= m_Capacity.load(std::memory_order_acquire)) {
    LOG_ALLOCATOR(
        "WARN", "HandleTable: Resolve failed - index out of bounds: " << index);
    return nullptr;
  }

  const HandleMetadata &meta = m_Metadata[index];

  // Generation check (prevents use-after-free)
  if (meta.Generation != generation) {
    LOG_ALLOCATOR("DEBUG",
                  "HandleTable: Resolve failed - stale handle (gen mismatch)");
    return nullptr;
  }

  return meta.Pointer;
}

bool HandleTable::Free(Handle H) noexcept {
  if (!H.IsValid()) {
    LOG_ALLOCATOR("WARN", "HandleTable: Attempted to free invalid handle");
    return false;
  }

  uint32_t index = H.GetIndex();
  uint32_t generation = H.GetGeneration();

  // Bounds check
  if (index >= m_Capacity.load(std::memory_order_acquire)) {
    LOG_ALLOCATOR("ERROR",
                  "HandleTable: Free failed - index out of bounds: " << index);
    return false;
  }

  HandleMetadata &meta = m_Metadata[index];

  // Generation check
  if (meta.Generation != generation) {
    LOG_ALLOCATOR(
        "WARN",
        "HandleTable: Free failed - generation mismatch (double free?)");
    return false;
  }

  // Clear pointer
  meta.Pointer = nullptr;

  // Increment generation (wraps around, but 0 is never used)
  meta.Generation++;
  if (meta.Generation == 0) {
    meta.Generation = 1;
  }

  // Add to free list (lock-free)
  while (true) {
    uint32_t currentHead = m_FreeListHead.load(std::memory_order_acquire);
    meta.NextFree = currentHead;

    if (m_FreeListHead.compare_exchange_weak(currentHead, index,
                                             std::memory_order_release,
                                             std::memory_order_acquire)) {
      // Successfully added to free list
      m_ActiveCount.fetch_sub(1, std::memory_order_relaxed);

      LOG_ALLOCATOR("DEBUG", "HandleTable: Freed handle - Index: " << index);
      return true;
    }

    // CAS failed, retry
  }
}

bool HandleTable::IsValid(Handle H) const noexcept {
  if (!H.IsValid()) {
    return false;
  }

  uint32_t index = H.GetIndex();
  uint32_t generation = H.GetGeneration();

  if (index >= m_Capacity.load(std::memory_order_acquire)) {
    return false;
  }

  return m_Metadata[index].Generation == generation;
}

bool HandleTable::Update(Handle H, void *NewPointer) noexcept {
  if (!H.IsValid()) {
    return false;
  }

  uint32_t index = H.GetIndex();
  uint32_t generation = H.GetGeneration();

  if (index >= m_Capacity.load(std::memory_order_acquire)) {
    return false;
  }

  HandleMetadata &meta = m_Metadata[index];

  // Generation check
  if (meta.Generation != generation) {
    return false;
  }

  // Update pointer
  meta.Pointer = NewPointer;

  LOG_ALLOCATOR("DEBUG", "HandleTable: Updated handle - Index: " << index);
  return true;
}

void HandleTable::Clear() noexcept {
  LOG_ALLOCATOR("INFO", "HandleTable: Clearing all handles");

  uint32_t capacity = m_Capacity.load(std::memory_order_acquire);

  // Reset all metadata
  for (uint32_t i = 0; i < capacity; ++i) {
    m_Metadata[i].Pointer = nullptr;
    m_Metadata[i].Generation++;
    if (m_Metadata[i].Generation == 0) {
      m_Metadata[i].Generation = 1;
    }
  }

  // Rebuild free list
  InitializeFreeList(0, capacity);

  m_ActiveCount.store(0, std::memory_order_release);

  LOG_ALLOCATOR("INFO", "HandleTable: Clear complete");
}

uint32_t HandleTable::GetActiveCount() const noexcept {
  return m_ActiveCount.load(std::memory_order_relaxed);
}

uint32_t HandleTable::GetCapacity() const noexcept {
  return m_Capacity.load(std::memory_order_relaxed);
}

float HandleTable::GetUtilization() const noexcept {
  uint32_t active = m_ActiveCount.load(std::memory_order_relaxed);
  uint32_t capacity = m_Capacity.load(std::memory_order_relaxed);
  return capacity > 0 ? (static_cast<float>(active) / capacity) : 0.0f;
}

} // namespace Allocator

// ============================================================================
// Standard Library Hash Implementation (Non-Template)
// ============================================================================
namespace std {
size_t
hash<Allocator::Handle>::operator()(const Allocator::Handle &H) const noexcept {
  return Allocator::Handle::Hash{}(H);
}
} // namespace std
