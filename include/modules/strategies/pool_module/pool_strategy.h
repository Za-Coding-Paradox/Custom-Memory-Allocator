#pragma once
#include <Core/core.h>
#include <modules/strategies/pool_module/pool_scopes.h>

namespace Allocator {

class PoolStrategy
{
public:
    PoolStrategy() = delete;
    ~PoolStrategy() = delete;

    static void Format(SlabDescriptor& SlabToInitialize, size_t ChunkSize) noexcept;

    static void Free(SlabDescriptor& SlabToFree, void* MemoryToFree) noexcept;

    [[nodiscard]] __attribute__((always_inline)) static inline void*
    Allocate(SlabDescriptor& SlabToAllocate) noexcept
    {
        auto& Head = SlabToAllocate.GetFreeListHeadAtomic();
        uintptr_t Current = Head.load(std::memory_order_acquire);

        while (Current != 0) {
            const uintptr_t Next = *reinterpret_cast<const uintptr_t*>(Current);

            if (Head.compare_exchange_weak(Current, Next, std::memory_order_acquire,
                                           std::memory_order_acquire)) {
                ALLOCATOR_DIAGNOSTIC({ SlabToAllocate.IncrementActiveSlots(); });
                return reinterpret_cast<void*>(Current);
            }
        }
        return nullptr;
    }

    [[nodiscard]] __attribute__((always_inline)) static inline bool
    CanFit(const SlabDescriptor& SlabToCheck) noexcept
    {
        return SlabToCheck.GetFreeListHead() != 0;
    }
};

} // namespace Allocator
