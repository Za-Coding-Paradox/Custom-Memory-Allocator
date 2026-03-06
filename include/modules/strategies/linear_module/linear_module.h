#pragma once

#include <modules/allocation_stats.h>
#include <modules/strategies/linear_module/linear_strategy.h>

namespace Allocator {

template <typename TContext> class LinearModuleThreadGuard;

template <typename TContext> class LinearStrategyModule
{
public:
    struct alignas(64) ThreadLocalData
    {
        SlabDescriptor* HeadSlab = nullptr;
        SlabDescriptor* ActiveSlab = nullptr;

        size_t ThreadAllocated = 0;
        size_t ThreadFreed = 0;
        size_t ThreadCount = 0;
        size_t ThreadPeak = 0;

        static constexpr size_t kDataSize = sizeof(SlabDescriptor*) * 2 + sizeof(size_t) * 4;

        static constexpr size_t kPadSize = (64 - (kDataSize % 64)) % 64;

        char Padding[kPadSize > 0 ? kPadSize : 1];
    };

    static_assert(sizeof(ThreadLocalData) % 64 == 0,
                  "ThreadLocalData size must be a multiple of 64 bytes (one cache line). "
                  "Update kDataSize or add more padding.");

private:
    [[nodiscard]] static __attribute__((always_inline)) inline ThreadLocalData& GetTLS() noexcept
    {
        static thread_local ThreadLocalData Context;
        return Context;
    }

    static thread_local LinearModuleThreadGuard<TContext> g_ThreadGuard;

    static std::atomic<SlabRegistry*> g_SlabRegistry;
    static ContextStats g_GlobalStats;
    static std::mutex g_ContextMutex;
    static std::vector<ThreadLocalData*> g_ThreadHeads;

    [[nodiscard]] static void* OverFlowAllocate(size_t AllocationSize,
                                                size_t AllocationAlignment) noexcept;
    static void GrowSlabChain() noexcept;
    static void RegisterThreadContext(ThreadLocalData* TLS) noexcept;
    static void UnregisterThreadContext(ThreadLocalData* TLS) noexcept;

    friend class LinearModuleThreadGuard<TContext>;
    friend class AllocatorEngine;

public:
    LinearStrategyModule() = delete;

    static void InitializeModule(SlabRegistry* RegistryInstance) noexcept;
    static void ShutdownModule() noexcept;
    static void ShutdownSystem() noexcept;

    static inline size_t GetThreadTotalUsed() noexcept
    {
        size_t TotalUsed = 0;
        ALLOCATOR_DIAGNOSTIC({
            auto& tls = GetTLS();
            SlabDescriptor* Current = tls.HeadSlab;
            while (Current != nullptr) {
                TotalUsed +=
                    static_cast<size_t>(Current->GetFreeListHead() - Current->GetSlabStart());
                if (Current == tls.ActiveSlab)
                    break;
                Current = Current->GetNextSlab();
            }
        });
        return TotalUsed;
    }

    static inline void FlushThreadStats() noexcept
    {
        ALLOCATOR_DIAGNOSTIC({
            auto& tls = GetTLS();
            if (tls.ThreadAllocated > 0 || tls.ThreadFreed > 0 || tls.ThreadCount > 0) {
                g_GlobalStats.BytesAllocated.fetch_add(tls.ThreadAllocated,
                                                       std::memory_order_relaxed);
                g_GlobalStats.BytesFreed.fetch_add(tls.ThreadFreed, std::memory_order_relaxed);
                g_GlobalStats.AllocationCount.fetch_add(tls.ThreadCount, std::memory_order_relaxed);

                tls.ThreadAllocated = 0;
                tls.ThreadFreed = 0;
                tls.ThreadCount = 0;
                tls.ThreadPeak = 0;
            }
        });
    }

    [[nodiscard]] __attribute__((always_inline)) static void*
    Allocate(size_t AllocationSize, size_t AllocationAlignment) noexcept
    {
        LOG_ALLOCATOR("DEBUG", "[L-ALLOC] Entering Allocate. Size: " << AllocationSize);

        (void)g_ThreadGuard;

        auto& tls = GetTLS();

        if (AllocationSize > g_ConstSlabSize) [[unlikely]] {
            LOG_ALLOCATOR("WARN", "[L-ALLOC] Size too large: " << AllocationSize);
            return nullptr;
        }

        if (tls.ActiveSlab == nullptr) [[unlikely]] {
            LOG_ALLOCATOR("DEBUG", "[L-ALLOC] No active slab. Growing...");
            GrowSlabChain();
            if (tls.ActiveSlab == nullptr) [[unlikely]] {
                LOG_ALLOCATOR("CRITICAL", "[L-ALLOC] Growth FAILED.");
                return nullptr;
            }
        }

        void* Result = nullptr;
        LOG_ALLOCATOR("DEBUG", "[L-ALLOC] Checking CanFit on: " << tls.ActiveSlab);

        if (LinearStrategy::CanFit(*tls.ActiveSlab, AllocationSize, AllocationAlignment))
            [[likely]] {
            Result = LinearStrategy::Allocate(*tls.ActiveSlab, AllocationSize, AllocationAlignment);
        }
        else {
            LOG_ALLOCATOR("DEBUG", "[L-ALLOC] Slab Full. Overflowing...");
            Result = OverFlowAllocate(AllocationSize, AllocationAlignment);
        }

        if (Result != nullptr) [[likely]] {
            LOG_ALLOCATOR("DEBUG", "[L-ALLOC] Success! Ptr: " << Result);

            ALLOCATOR_DIAGNOSTIC({
                tls.ThreadCount++;
                tls.ThreadAllocated += AllocationSize;

                size_t CurrentUsed = GetThreadTotalUsed();
                if (CurrentUsed > tls.ThreadPeak)
                    tls.ThreadPeak = CurrentUsed;
            });
        }

        return Result;
    }

    static ContextStats& GetGlobalStats() noexcept { return g_GlobalStats; }

    static void Reset() noexcept
        requires(!TContext::IsRewindable);
    static void RewindState(SlabDescriptor* SavedSlab, uintptr_t SavedOffset) noexcept
        requires(TContext::IsRewindable);
    [[nodiscard]] static std::pair<SlabDescriptor*, uintptr_t> GetCurrentState() noexcept
        requires(TContext::IsRewindable);
};

template <typename TContext> class LinearModuleThreadGuard
{
public:
    LinearModuleThreadGuard() { LOG_ALLOCATOR("DEBUG", "[TLS] Guard Constructed for thread."); }
    ~LinearModuleThreadGuard() noexcept
    {
        LinearStrategyModule<TContext>::FlushThreadStats();
        LinearStrategyModule<TContext>::ShutdownModule();
    }
};

template <typename TContext> class LinearScopedMarker
{
    static_assert(TContext::IsRewindable, "Marker requires Rewindable context.");
    SlabDescriptor* m_MarkedSlab;
    uintptr_t m_MarkedOffset;
    bool m_HasState;

public:
    LinearScopedMarker() noexcept;
    ~LinearScopedMarker() noexcept;
    void Commit() noexcept { m_HasState = false; }
};

template <typename TContext>
inline LinearScopedMarker<TContext>::LinearScopedMarker() noexcept : m_HasState(true)
{
    auto State = LinearStrategyModule<TContext>::GetCurrentState();
    m_MarkedSlab = State.first;
    m_MarkedOffset = State.second;
}

template <typename TContext> inline LinearScopedMarker<TContext>::~LinearScopedMarker() noexcept
{
    if (m_HasState && m_MarkedSlab) {
        LinearStrategyModule<TContext>::RewindState(m_MarkedSlab, m_MarkedOffset);
    }
}

} // namespace Allocator
