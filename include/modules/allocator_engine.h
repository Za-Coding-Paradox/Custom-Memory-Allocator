#pragma once

#include <modules/allocator_handle_system.h>
#include <modules/strategies/linear_module/linear_module.h>

namespace Allocator {

const static size_t g_GlobalContextBasedGeneralAlignmentSize = 16;

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

    void* Ptr = LinearStrategyModule<TScope>::Allocate(Size, Alignment);

    if (Ptr == nullptr) [[unlikely]] {
      ReportError("Allocation failed (OOM)", std::source_location::current());
    }

    return Ptr;
  }

  template <typename T, typename TScope>
  [[nodiscard]]
  Handle AllocateWithHandle() noexcept {
    static_assert(TScope::SupportsHandles,
                  "Allocator Violation: This Scope/Strategy does not support Handles.");

    void* Memory = Allocate<TScope>(sizeof(T), alignof(T));

    if (Memory == nullptr) [[unlikely]] {
      return g_InvalidHandle;
    }
    return m_HandleTable.Allocate(Memory);
  }

  template <typename T>
  [[nodiscard]] __attribute__((always_inline)) T* ResolveHandle(Handle InHandle) const noexcept {
    return static_cast<T*>(m_HandleTable.Resolve(InHandle));
  }

  bool FreeHandle(Handle InHandle) noexcept { return m_HandleTable.Free(InHandle); }

  template <typename TScope> void Reset() noexcept {
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

    ContextStats::Snapshot Snap = LinearStrategyModule<TScope>::GetGlobalStats().GetSnapshot();

    std::cout << "\n[" << ScopeName << " Stats]\n";
    std::cout << "  Allocated : " << FormatBytes(Snap.Allocated) << "\n";
    std::cout << "  Current   : " << FormatBytes(Snap.Current) << "\n";
    std::cout << "  Peak      : " << FormatBytes(Snap.Peak) << "\n";
    std::cout << "  Count     : " << Snap.Count << "\n";
  }

  void GenerateFullReport() const noexcept;

private:
  void ReportError(const char* Msg, std::source_location Loc) const noexcept;
  static std::string FormatBytes(size_t Bytes) noexcept;
};

} // namespace Allocator
