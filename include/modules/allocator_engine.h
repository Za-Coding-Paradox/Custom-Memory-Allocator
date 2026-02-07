#pragma once

#include <Core/core.h>
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
        LOG_ALLOCATOR("DEBUG", "Engine: Allocating " << Size << " bytes in Linear Scope.");
        static_assert(!ScopeTraits<TScope>::SupportsHandles,
                      "Engine: Use AllocateWithHandle for Pool-based Scopes.");

        void* ptr = RawAllocateInternal<TScope>(Size, Alignment);
        if (!ptr)
            LOG_ALLOCATOR("ERROR", "Engine: Linear Allocation FAILED for size " << Size);
        return ptr;
    }

    template <typename TType, typename TScope = PoolScope<TType>>
    [[nodiscard]] __attribute__((always_inline)) inline Handle AllocateWithHandle() noexcept
    {
        LOG_ALLOCATOR("DEBUG",
                      "Engine: Allocating Handle-based object (Size: " << sizeof(TType) << ")");
        static_assert(ScopeTraits<TScope>::SupportsHandles,
                      "Engine: Linear Scopes do not support Handles.");

        void* Memory = RawAllocateInternal<TScope>(sizeof(TType), alignof(TType));
        if (Memory == nullptr) [[unlikely]] {
            LOG_ALLOCATOR("ERROR", "Engine: Raw Memory Allocation for Handle FAILED.");
            return g_InvalidHandle;
        }

        Handle h = m_HandleTable.Allocate(Memory);
        if (!h.IsValid())
            LOG_ALLOCATOR("ERROR", "Engine: Handle Table Allocation FAILED.");
        return h;
    }

    template <typename TType, typename TScope = PoolScope<TType>>
    __attribute__((always_inline)) inline bool FreeHandle(Handle InHandle) noexcept
    {
        LOG_ALLOCATOR("DEBUG", "Engine: Freeing Handle " << InHandle.GetIndex());
        static_assert(ScopeTraits<TScope>::SupportsHandles,
                      "Engine: Only Pools support individual Free().");

        void* Memory = m_HandleTable.Resolve(InHandle);
        if (Memory != nullptr) [[likely]] {
            using Bucket = typename PoolMap<TScope::g_BucketSize>::Type;
            PoolModule<Bucket>::Free(Memory);
        }
        else {
            LOG_ALLOCATOR("WARN", "Engine: Attempted to free an unresolvable or stale handle.");
        }
        return m_HandleTable.Free(InHandle);
    }

    template <typename T>
    [[nodiscard]] __attribute__((always_inline)) inline T*
    ResolveHandle(Handle InHandle) const noexcept
    {
        void* ptr = m_HandleTable.Resolve(InHandle);
        if (!ptr && InHandle.IsValid()) {
            LOG_ALLOCATOR(
                "DEBUG",
                "Engine: Handle resolution returned nullptr (Handle valid, entry empty or stale).");
        }
        return static_cast<T*>(ptr);
    }

    template <typename TScope> void Reset() noexcept
    {
        LOG_ALLOCATOR("INFO", "Engine: Resetting scope memory.");
        static_assert(!ScopeTraits<TScope>::SupportsHandles, "Engine: Pools cannot be Reset().");
        if constexpr (ScopeTraits<TScope>::IsRewindable) {
            LinearStrategyModule<TScope>::RewindState(nullptr, 0);
        }
        else {
            LinearStrategyModule<TScope>::Reset();
        }
    }

    void* GetRegistryAddress() { return &m_Registry; }

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
