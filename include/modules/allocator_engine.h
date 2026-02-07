#pragma once

#include <modules/allocator_handle_system.h>
#include <modules/strategies/linear_module/linear_module.h>
#include <modules/strategies/pool_module/pool_module.h>

namespace Allocator {

const static size_t g_GlobalContextBasedGeneralAlignmentSize = 16;

template <typename T> struct ScopeTraits
{
    static constexpr bool SupportsHandles =
        requires { T::SupportsHandles; } ? T::SupportsHandles : false;
    static constexpr bool IsRewindable = requires { T::IsRewindable; } ? T::IsRewindable : false;
};

class AllocatorEngine
{
private:
    SlabRegistry m_Registry;
    HandleTable m_HandleTable;

public:
    AllocatorEngine(size_t SlabSize, size_t ArenaSize);
    ~AllocatorEngine();

    void Initialize();
    void Shutdown();

    template <typename TScope>
    [[nodiscard]] __attribute__((always_inline)) inline void*
    Allocate(size_t Size, size_t Alignment = g_GlobalContextBasedGeneralAlignmentSize) noexcept
    {
        static_assert(!ScopeTraits<TScope>::SupportsHandles,
                      "Engine: Use AllocateWithHandle for Pool-based Scopes.");
        return RawAllocateInternal<TScope>(Size, Alignment);
    }

    template <typename TType, typename TScope = PoolScope<TType>>
    [[nodiscard]] __attribute__((always_inline)) inline Handle AllocateWithHandle() noexcept
    {
        static_assert(ScopeTraits<TScope>::SupportsHandles,
                      "Engine: Linear Scopes do not support Handles.");

        void* Memory = RawAllocateInternal<TScope>(sizeof(TType), alignof(TType));
        if (Memory == nullptr) [[unlikely]]
            return g_InvalidHandle;

        return m_HandleTable.Allocate(Memory);
    }

    template <typename TType, typename TScope = PoolScope<TType>>
    __attribute__((always_inline)) inline bool FreeHandle(Handle InHandle) noexcept
    {
        static_assert(ScopeTraits<TScope>::SupportsHandles,
                      "Engine: Only Pools support individual Free().");

        void* Memory = m_HandleTable.Resolve(InHandle);
        if (Memory != nullptr) [[likely]] {
            using Bucket = typename PoolMap<TScope::g_BucketSize>::Type;
            PoolModule<Bucket>::Free(Memory);
        }
        return m_HandleTable.Free(InHandle);
    }

    template <typename T>
    [[nodiscard]] __attribute__((always_inline)) inline T*
    ResolveHandle(Handle InHandle) const noexcept
    {
        return static_cast<T*>(m_HandleTable.Resolve(InHandle));
    }

    template <typename TScope> void Reset() noexcept
    {
        static_assert(!ScopeTraits<TScope>::SupportsHandles, "Engine: Pools cannot be Reset().");
        if constexpr (ScopeTraits<TScope>::IsRewindable) {
            LinearStrategyModule<TScope>::RewindState(nullptr, 0);
        }
        else {
            LinearStrategyModule<TScope>::Reset();
        }
    }

    template <typename TScope> void PrintStats(const char* ScopeName) const noexcept;
    void GenerateFullReport() const noexcept;

private:
    template <typename TScope>
    __attribute__((always_inline)) inline void* RawAllocateInternal(size_t Size,
                                                                    size_t Alignment) noexcept
    {
        if (Size == 0) [[unlikely]]
            return nullptr;

        if constexpr (ScopeTraits<TScope>::SupportsHandles) {
            using Bucket = typename PoolMap<TScope::g_BucketSize>::Type;
            return PoolModule<Bucket>::Allocate();
        }
        else {
            return LinearStrategyModule<TScope>::Allocate(Size, Alignment);
        }
    }
};

} // namespace Allocator
