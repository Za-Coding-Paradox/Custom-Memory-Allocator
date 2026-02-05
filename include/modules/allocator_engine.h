#pragma once

#include <modules/allocator_handle_system.h>
#include <modules/strategies/linear_module/linear_module.h>

namespace Allocator {

const static size_t g_GlobalContextBasedGeneralAlignmentSize = 16;

struct ContextStats {
  std::atomic<size_t> BytesAllocated{0};
  std::atomic<size_t> BytesFreed{0};
  std::atomic<size_t> AllocationCount{0};
  std::atomic<size_t> PeakUsage{0};

  struct Snapshot {
    size_t Allocated;
    size_t Freed;
    size_t Count;
    size_t Peak;
    size_t Current;
  };

  [[nodiscard]] Snapshot GetSnapshot() const noexcept {
    size_t alloc = BytesAllocated.load(std::memory_order_relaxed);
    size_t freed = BytesFreed.load(std::memory_order_relaxed);
    return Snapshot{.Allocated = alloc,
                    .Freed = freed,
                    .Count = AllocationCount.load(std::memory_order_relaxed),
                    .Peak = PeakUsage.load(std::memory_order_relaxed),
                    .Current = (alloc > freed) ? (alloc - freed) : 0};
  }
};

class AllocatorEngine {
private:
  SlabRegistry m_Registry;
  HandleTable m_HandleTable;

public:
  AllocatorEngine(size_t SlabSize, size_t ArenaSize);
  ~AllocatorEngine();

  void Initialize();
  void Shutdown();

  template <typename TScope>
  [[nodiscard]] __attribute__((always_inline)) void*
  Allocate(size_t Size, size_t Alignment = g_GlobalContextBasedGeneralAlignmentSize) noexcept {
    if (Size == 0) [[unlikely]] {
      return nullptr;
    }

    void* ptr = LinearStrategyModule<TScope>::Allocate(Size, Alignment);

    if (ptr == nullptr) [[unlikely]] {
      ReportError("Allocation failed (OOM)", std::source_location::current());
    }

    return ptr;
  }

  template <typename T, typename TScope>
  [[nodiscard]]
  Handle AllocateWithHandle() noexcept {
    static_assert(TScope::SupportsHandles,
                  "Allocator Violation: This Scope/Strategy does not support Handles.");

    void* memory = Allocate<TScope>(sizeof(T), alignof(T));

    if (memory == nullptr) [[unlikely]] {
      return INVALID_HANDLE;
    }

    return m_HandleTable.Allocate(memory);
  }

  template <typename T>
  [[nodiscard]] __attribute__((always_inline)) T* ResolveHandle(Handle InHandle) const noexcept {
    return static_cast<T*>(m_HandleTable.Resolve(InHandle));
  }

  bool FreeHandle(Handle InHandle) noexcept { return m_HandleTable.Free(InHandle); }

  template <typename TScope> void Reset() noexcept {
    // Runtime check: Ensure we don't reset a rewindable scope incorrectly
    if constexpr (TScope::IsRewindable) {
      ReportError("Attempted to Reset() a Rewindable Scope. Use RestoreState() instead.",
                  std::source_location::current());
      return;
    }
    LinearStrategyModule<TScope>::Reset();
  }

  template <typename TScope>
  [[nodiscard]] std::pair<SlabDescriptor*, uintptr_t> SaveState() noexcept {
    static_assert(TScope::IsRewindable, "Scope is not rewindable.");
    return LinearStrategyModule<TScope>::GetCurrentState();
  }

  template <typename TScope> void RestoreState(SlabDescriptor* Slab, uintptr_t Offset) noexcept {
    static_assert(TScope::IsRewindable, "Scope is not rewindable.");
    LinearStrategyModule<TScope>::RewindState(Slab, Offset);
  }

  template <typename TScope> void PrintStats(const char* ScopeName) const noexcept {
    LinearStrategyModule<TScope>::FlushThreadStats();

    ContextStats::Snapshot snap = LinearStrategyModule<TScope>::GetGlobalStats().GetSnapshot();

    std::cout << "\n[" << ScopeName << " Stats]\n";
    std::cout << "  Allocated : " << FormatBytes(snap.Allocated) << "\n";
    std::cout << "  Current   : " << FormatBytes(snap.Current) << "\n";
    std::cout << "  Peak      : " << FormatBytes(snap.Peak) << "\n";
    std::cout << "  Count     : " << snap.Count << "\n";
  }

  void GenerateFullReport() const noexcept;

private:
  void ReportError(const char* Msg, std::source_location Loc) const noexcept;
  static std::string FormatBytes(size_t Bytes) noexcept;
};

} // namespace Allocator
