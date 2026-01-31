#pragma once

#include <Core/core.h>
#include <atomic>
#include <mutex>
#include <utilities/allocator_utility.h>
#include <vector>

namespace Allocator {

// ============================================================================
// Handle Class
// ============================================================================
class Handle {
private:
  uint64_t m_Packed;

public:
  // These constructors MUST be in the header to support 'constexpr' constants.
  constexpr Handle() noexcept : m_Packed(0) {}
  constexpr Handle(uint32_t Index, uint32_t Generation) noexcept
      : m_Packed((static_cast<uint64_t>(Generation) << 32) | Index) {}

  // Declarations only (Definitions moved to .cpp)
  [[nodiscard]] uint32_t GetIndex() const noexcept;
  [[nodiscard]] uint32_t GetGeneration() const noexcept;
  [[nodiscard]] uint64_t GetPacked() const noexcept;
  [[nodiscard]] bool IsValid() const noexcept;

  [[nodiscard]] bool operator==(const Handle &Other) const noexcept;
  [[nodiscard]] bool operator!=(const Handle &Other) const noexcept;
  [[nodiscard]] bool operator<(const Handle &Other) const noexcept;

  struct Hash {
    [[nodiscard]] size_t operator()(const Handle &H) const noexcept;
  };
};

// Invalid handle constant (Requires the constexpr constructor above)
inline constexpr Handle INVALID_HANDLE = Handle();

// ============================================================================
// Handle Metadata
// ============================================================================
struct HandleMetadata {
  void *Pointer;
  uint32_t Generation;
  uint32_t NextFree;

  // Definition moved to .cpp
  HandleMetadata() noexcept;
};

// ============================================================================
// Handle Table
// ============================================================================
class HandleTable {
private:
  static constexpr uint32_t INITIAL_CAPACITY = 1024;
  static constexpr uint32_t MAX_CAPACITY = 1024 * 1024;
  static constexpr uint32_t FREE_LIST_END = 0xFFFFFFFF;

  std::vector<HandleMetadata> m_Metadata;
  std::atomic<uint32_t> m_FreeListHead;
  std::atomic<uint32_t> m_Capacity;
  std::atomic<uint32_t> m_ActiveCount;
  mutable std::mutex m_GrowthMutex;

  void InitializeFreeList(uint32_t Start, uint32_t End) noexcept;
  bool GrowCapacity() noexcept;

public:
  explicit HandleTable(uint32_t InitialCapacity = INITIAL_CAPACITY) noexcept;
  ~HandleTable() noexcept = default;

  HandleTable(const HandleTable &) = delete;
  HandleTable &operator=(const HandleTable &) = delete;
  HandleTable(HandleTable &&) = delete;
  HandleTable &operator=(HandleTable &&) = delete;

  // Core Operations
  [[nodiscard]] Handle Allocate(void *Pointer) noexcept;
  [[nodiscard]] void *Resolve(Handle H) const noexcept;
  bool Free(Handle H) noexcept;
  [[nodiscard]] bool IsValid(Handle H) const noexcept;
  bool Update(Handle H, void *NewPointer) noexcept;

  // Statistics
  [[nodiscard]] uint32_t GetActiveCount() const noexcept;
  [[nodiscard]] uint32_t GetCapacity() const noexcept;
  [[nodiscard]] float GetUtilization() const noexcept;

  void Clear() noexcept;
};

// ============================================================================
// Typed Handle (Templates MUST be in header)
// ============================================================================
template <typename T> class TypedHandle {
private:
  Handle m_Handle;

public:
  TypedHandle() noexcept : m_Handle(INVALID_HANDLE) {}
  explicit TypedHandle(Handle H) noexcept : m_Handle(H) {}

  [[nodiscard]] Handle GetHandle() const noexcept { return m_Handle; }
  [[nodiscard]] bool IsValid() const noexcept { return m_Handle.IsValid(); }

  [[nodiscard]] bool operator==(const TypedHandle &Other) const noexcept {
    return m_Handle == Other.m_Handle;
  }

  [[nodiscard]] bool operator!=(const TypedHandle &Other) const noexcept {
    return m_Handle != Other.m_Handle;
  }

  [[nodiscard]] T *Resolve(const HandleTable &Table) const noexcept {
    return static_cast<T *>(Table.Resolve(m_Handle));
  }

  struct Hash {
    [[nodiscard]] size_t operator()(const TypedHandle &H) const noexcept {
      return Handle::Hash{}(H.m_Handle);
    }
  };
};

// ============================================================================
// Template Helpers
// ============================================================================
template <typename T>
[[nodiscard]] inline TypedHandle<T> AllocateTyped(HandleTable &Table,
                                                  T *Pointer) noexcept {
  return TypedHandle<T>(Table.Allocate(static_cast<void *>(Pointer)));
}

template <typename T>
[[nodiscard]] inline T *ResolveTyped(const HandleTable &Table,
                                     TypedHandle<T> H) noexcept {
  return H.Resolve(Table);
}

} // namespace Allocator

// ============================================================================
// Standard Library Hash Support
// ============================================================================
namespace std {
template <> struct hash<Allocator::Handle> {
  size_t operator()(const Allocator::Handle &H) const noexcept;
};

template <typename T> struct hash<Allocator::TypedHandle<T>> {
  size_t operator()(const Allocator::TypedHandle<T> &H) const noexcept {
    // "typename" is required here because TypedHandle<T> depends on T
    return typename Allocator::TypedHandle<T>::Hash{}(H);
  }
};
} // namespace std
