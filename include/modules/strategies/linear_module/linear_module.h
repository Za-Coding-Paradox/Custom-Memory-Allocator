#pragma once

#include <modules/allocation_stats.h>
#include <modules/strategies/linear_module/linear_strategy.h>

namespace Allocator {

template <typename TContext> class LinearModuleThreadGuard;

template <typename TContext> class LinearStrategyModule
{
private:
    static thread_local SlabDescriptor* g_HeadSlab;
    static thread_local SlabDescriptor* g_ActiveSlab;
    static thread_local LinearModuleThreadGuard<TContext> g_ThreadGuard;

    static thread_local size_t g_ThreadAllocated;
    static thread_local size_t g_ThreadFreed;
    static thread_local size_t g_ThreadCount;
    static thread_local size_t g_ThreadPeak;

    static std::atomic<SlabRegistry*> g_SlabRegistry;
    static ContextStats g_GlobalStats;
    static std::mutex g_ContextMutex;
    static std::vector<SlabDescriptor**> g_ThreadHeads;

    [[nodiscard]] static void* OverFlowAllocate(size_t AllocationSize,
                                                size_t AllocationAlignment) noexcept;
    static void GrowSlabChain() noexcept;

    static size_t GetThreadTotalUsed() noexcept;

    static void RegisterThreadContext() noexcept;
    static void UnregisterThreadContext() noexcept;

    friend class LinearModuleThreadGuard<TContext>;
    friend class AllocatorEngine;

public:
    LinearStrategyModule() = delete;

    static void InitializeModule(SlabRegistry* RegistryInstance) noexcept;
    static void ShutdownModule() noexcept;
    static void ShutdownSystem() noexcept;

    [[nodiscard]] __attribute__((always_inline)) static void*
    Allocate(size_t AllocationSize, size_t AllocationAlignment) noexcept
    {
        (void)g_ThreadGuard;

        if (AllocationSize > g_ConstSlabSize) [[unlikely]]
            return nullptr;

        if (g_ActiveSlab == nullptr) [[unlikely]] {
            GrowSlabChain();
            if (g_ActiveSlab == nullptr)
                return nullptr;
        }

        void* Result = nullptr;

        if (LinearStrategy::CanFit(*g_ActiveSlab, AllocationSize, AllocationAlignment)) [[likely]] {
            const uintptr_t HeadBefore = g_ActiveSlab->GetFreeListHead();
            Result = LinearStrategy::Allocate(*g_ActiveSlab, AllocationSize, AllocationAlignment);
            const uintptr_t HeadAfter = g_ActiveSlab->GetFreeListHead();

            g_ThreadAllocated += static_cast<size_t>(HeadAfter - HeadBefore);
        }
        else {
            Result = OverFlowAllocate(AllocationSize, AllocationAlignment);
        }

        if (Result != nullptr) [[likely]] {
            g_ThreadCount++;

            ALLOCATOR_DIAGNOSTIC({
                size_t CurrentUsed = GetThreadTotalUsed();
                if (CurrentUsed > g_ThreadPeak) {
                    g_ThreadPeak = CurrentUsed;
                }
            });
        }
        return Result;
    }

    static void FlushThreadStats() noexcept;
    static ContextStats& GetGlobalStats() noexcept { return g_GlobalStats; }

    static void Free(SlabDescriptor&, void*) noexcept = delete;

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
    LinearModuleThreadGuard() = default;
    ~LinearModuleThreadGuard() noexcept
    {
        LinearStrategyModule<TContext>::FlushThreadStats();
        if (LinearStrategyModule<TContext>::g_HeadSlab != nullptr) {
            LinearStrategyModule<TContext>::ShutdownModule();
        }
    }
};

template <typename TContext> class LinearScopedMarker
{
    static_assert(TContext::IsRewindable, "LinearScopedMarker requires a Rewindable context.");

    SlabDescriptor* m_MarkedSlab;
    uintptr_t m_MarkedOffset;
    bool m_HasState;

public:
    LinearScopedMarker() noexcept;
    ~LinearScopedMarker() noexcept;

    __attribute__((always_inline)) void Commit() noexcept { m_HasState = false; }
};

} // namespace Allocator
