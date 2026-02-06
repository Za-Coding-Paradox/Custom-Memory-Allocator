#pragma once

#include <modules/allocator_handle_system.h>
#include <modules/strategies/linear_module/linear_module.h>
#include <modules/strategies/pool_module/pool_module.h>

namespace Allocator {

const static size_t g_GlobalContextBasedGeneralAlignmentSize = 16;

template <typename T> struct ScopeTraits
{
    static constexpr bool SupportsHandles = []() {
        if constexpr (requires { T::SupportsHandles; }) {
            return T::SupportsHandles;
        }
        return false;
    }();

    static constexpr bool IsRewindable = []() {
        if constexpr (requires { T::IsRewindable; }) {
            return T::IsRewindable;
        }
        return false;
    }();
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
    [[nodiscard]] __attribute__((always_inline)) void*
    Allocate(size_t Size, size_t Alignment = g_GlobalContextBasedGeneralAlignmentSize) noexcept
    {
        static_assert(!ScopeTraits<TScope>::SupportsHandles,
                      "Engine Violation: Use AllocateWithHandle for Pool-based Scopes.");
        return RawAllocateInternal<TScope>(Size, Alignment);
    }

    template <typename T, typename TScope> [[nodiscard]] Handle AllocateWithHandle() noexcept
    {
        static_assert(ScopeTraits<TScope>::SupportsHandles,
                      "Engine Violation: Linear Scopes do not support Handle-based allocation.");

        void* Memory = RawAllocateInternal<TScope>(sizeof(T), alignof(T));
        if (Memory == nullptr) [[unlikely]] {
            return g_InvalidHandle;
        }
        return m_HandleTable.Allocate(Memory);
    }

    template <typename TScope> bool FreeHandle(Handle InHandle) noexcept
    {
        static_assert(ScopeTraits<TScope>::SupportsHandles,
                      "Engine Violation: Only Pools support individual Free().");

        void* Memory = m_HandleTable.Resolve(InHandle);
        if (Memory != nullptr) {
            using Bucket = typename PoolMap<TScope::g_BucketSize>::Type;
            PoolModule<Bucket>::Free(Memory);
        }
        return m_HandleTable.Free(InHandle);
    }

    template <typename T>
    [[nodiscard]] __attribute__((always_inline)) T* ResolveHandle(Handle InHandle) const noexcept
    {
        return static_cast<T*>(m_HandleTable.Resolve(InHandle));
    }

    template <typename TScope> void Reset() noexcept
    {
        static_assert(!ScopeTraits<TScope>::SupportsHandles,
                      "Pools cannot be Reset(); they must be Freed individually.");

        if constexpr (ScopeTraits<TScope>::IsRewindable) {
            LinearStrategyModule<TScope>::RewindState(nullptr, 0);
        }
        else {
            LinearStrategyModule<TScope>::Reset();
        }
    }

    template <typename TScope>
    [[nodiscard]] std::pair<SlabDescriptor*, uintptr_t> SaveState() noexcept
    {
        static_assert(ScopeTraits<TScope>::IsRewindable, "Scope does not support SaveState.");
        return LinearStrategyModule<TScope>::GetCurrentState();
    }

    template <typename TScope> void RestoreState(SlabDescriptor* Slab, uintptr_t Offset) noexcept
    {
        static_assert(ScopeTraits<TScope>::IsRewindable, "Scope does not support RestoreState.");
        LinearStrategyModule<TScope>::RewindState(Slab, Offset);
    }

    template <typename TScope> void PrintStats(const char* ScopeName) const noexcept;
    void GenerateFullReport() const noexcept;

private:
    template <typename TScope> void* RawAllocateInternal(size_t Size, size_t Alignment) noexcept
    {
        if (Size == 0) [[unlikely]] {
            return nullptr;
        }

        void* Ptr = nullptr;
        if constexpr (ScopeTraits<TScope>::SupportsHandles) {
            using Bucket = typename PoolMap<TScope::g_BucketSize>::Type;
            Ptr = PoolModule<Bucket>::Allocate();
        }
        else {
            Ptr = LinearStrategyModule<TScope>::Allocate(Size, Alignment);
        }
        return Ptr;
    }

    void ReportError(const char* Msg, std::source_location Loc) const noexcept;
    static std::string FormatBytes(size_t Bytes) noexcept;
};

} // namespace Allocator
