#pragma once

#include <modules/allocator_handle_system.h>
#include <modules/strategies/linear_module/linear_module.h>

namespace Allocator {

static size_t g_AllocatorEngineBaseAlignment = 16;

class IAllocationStrategy {
public:
  virtual ~IAllocationStrategy() noexcept = default;

  virtual void* Allocate(size_t Size, size_t Alignment = 16) noexcept = 0;
  virtual void Free(void* Pointer) noexcept = 0;
  virtual void Reset() noexcept = 0;

  virtual const char* GetName() const noexcept = 0;
  virtual bool SupportsIndividualFree() const noexcept = 0;
  virtual bool SupportsRewind() const noexcept = 0;

  virtual size_t GetTotalAllocated() const noexcept = 0;
  virtual size_t GetPeakAllocated() const noexcept = 0;
  virtual size_t GetAllocationCount() const noexcept = 0;
};

enum class AllocatorContextType : uint8_t { Frame, Level, Global, Temporary, Persistent };

struct AllocationStats {
  std::atomic<size_t> TotalAllocated{0};
  std::atomic<size_t> PeakAllocated{0};
  std::atomic<size_t> AllocationCount{0};
  std::atomic<size_t> FreeCount{0};
  std::atomic<size_t> ResetCount{0};

  void RecordAllocation(size_t Size) noexcept;
  void RecordFree(size_t Size) noexcept;
  void RecordReset() noexcept;
  void Clear() noexcept;

  void Publish(size_t LocalAllocated, size_t LocalPeak, size_t LocalCount) noexcept;
};

class AllocatorEngine {
private:
  SlabRegistry m_Registry;
  HandleTable m_HandleTable;
  std::unordered_map<AllocatorContextType, AllocationStats> m_ContextStats;

  std::atomic<bool> m_IsInitialized{false};
  mutable std::mutex m_InitMutex;
  size_t m_SlabSize;
  size_t m_ArenaSize;

public:
  explicit AllocatorEngine(size_t SlabSize = g_ConstSlabSize, size_t ArenaSize = g_ConstArenaSize,
                           uint32_t HandleCapacity = 1024) noexcept;

  ~AllocatorEngine() noexcept;

  AllocatorEngine(const AllocatorEngine&) = delete;
  AllocatorEngine& operator=(const AllocatorEngine&) = delete;
  AllocatorEngine(AllocatorEngine&&) = delete;
  AllocatorEngine& operator=(AllocatorEngine&&) = delete;

  void Initialize() noexcept;
  void Shutdown() noexcept;

  template <typename TContext>
  [[nodiscard]] void* Allocate(size_t Size,
                               size_t Alignment = g_AllocatorEngineBaseAlignment) noexcept {
    static_assert(std::is_same_v<TContext, FrameLoad> || std::is_same_v<TContext, LevelLoad> ||
                      std::is_same_v<TContext, GlobalLoad>,
                  "Invalid Allocator Context Type");

    if (!m_IsInitialized.load(std::memory_order_acquire)) [[unlikely]] {
      return nullptr;
    }

    return LinearStrategyModule<TContext>::Allocate(Size, Alignment);
  }

  template <typename TContext>
  void Reset() noexcept
    requires(!TContext::IsRewindable)
  {
    LinearStrategyModule<TContext>::Reset();
  }

  template <typename T, typename TContext>
  [[nodiscard]] TypedHandle<T> AllocateWithHandle(size_t Count = 1) noexcept {
    size_t totalSize = sizeof(T) * Count;
    void* ptr = Allocate<TContext>(totalSize, alignof(T));

    if (ptr == nullptr)
      return TypedHandle<T>(INVALID_HANDLE);

    Handle handle = m_HandleTable.Allocate(ptr);
    if (!handle.IsValid())
      return TypedHandle<T>(INVALID_HANDLE);

    return TypedHandle<T>(handle);
  }

  template <typename T> [[nodiscard]] T* ResolveHandle(TypedHandle<T> Handle) const noexcept {
    return Handle.Resolve(m_HandleTable);
  }

  template <typename T> bool FreeHandle(TypedHandle<T> Handle) noexcept {
    return m_HandleTable.Free(Handle.GetHandle());
  }

  template <typename TContext>
  [[nodiscard]] std::pair<SlabDescriptor*, uintptr_t> SaveState() noexcept
    requires(TContext::IsRewindable)
  {
    return LinearStrategyModule<TContext>::GetCurrentState();
  }

  template <typename TContext>
  void RestoreState(SlabDescriptor* Slab, uintptr_t Offset) noexcept
    requires(TContext::IsRewindable)
  {
    LinearStrategyModule<TContext>::RewindState(Slab, Offset);
  }

  template <typename T, typename TContext> class ScopedAllocation {
  private:
    AllocatorEngine& m_Engine;
    TypedHandle<T> m_Handle;

  public:
    ScopedAllocation(AllocatorEngine& Engine, size_t Count = 1) noexcept
        : m_Engine(Engine), m_Handle(Engine.AllocateWithHandle<T, TContext>(Count)) {}

    ~ScopedAllocation() noexcept {
      if (m_Handle.IsValid())
        m_Engine.FreeHandle(m_Handle);
    }

    ScopedAllocation(const ScopedAllocation&) = delete;
    ScopedAllocation& operator=(const ScopedAllocation&) = delete;

    [[nodiscard]] T* Get() noexcept { return m_Engine.ResolveHandle(m_Handle); }
    [[nodiscard]] TypedHandle<T> GetHandle() const noexcept { return m_Handle; }
    [[nodiscard]] bool IsValid() const noexcept { return m_Handle.IsValid(); }

    [[nodiscard]] TypedHandle<T> Release() noexcept {
      TypedHandle<T> handle = m_Handle;
      m_Handle = TypedHandle<T>(INVALID_HANDLE);
      return handle;
    }
  };

  template <typename T, typename TContext>
  [[nodiscard]] ScopedAllocation<T, TContext> AllocateScoped(size_t Count = 1) noexcept {
    return ScopedAllocation<T, TContext>(*this, Count);
  }

  [[nodiscard]] const AllocationStats& GetStats(AllocatorContextType Context) const noexcept;
  [[nodiscard]] size_t GetTotalAllocated() const noexcept;
  [[nodiscard]] size_t GetPeakAllocated() const noexcept;

  void PrintStatistics() const noexcept;
  void ClearStatistics() noexcept;

  [[nodiscard]] SlabRegistry& GetRegistry() noexcept;
  [[nodiscard]] const SlabRegistry& GetRegistry() const noexcept;
  [[nodiscard]] HandleTable& GetHandleTable() noexcept;
  [[nodiscard]] const HandleTable& GetHandleTable() const noexcept;
  [[nodiscard]] bool IsInitialized() const noexcept;

private:
  template <typename TContext> [[nodiscard]] static AllocatorContextType GetContextType() noexcept {
    if constexpr (std::is_same_v<TContext, FrameLoad>)
      return AllocatorContextType::Frame;
    if constexpr (std::is_same_v<TContext, LevelLoad>)
      return AllocatorContextType::Level;
    if constexpr (std::is_same_v<TContext, GlobalLoad>)
      return AllocatorContextType::Global;
    return AllocatorContextType::Frame;
  }

  [[nodiscard]] static const char* GetContextName(AllocatorContextType Type) noexcept;
};

} // namespace Allocator
