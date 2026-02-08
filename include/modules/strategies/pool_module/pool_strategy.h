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
        std::lock_guard<std::mutex> Lock(SlabToAllocate.GetMutex());

        const uintptr_t AvailableAddress = SlabToAllocate.GetFreeListHead();

        if (AvailableAddress == 0) [[unlikely]] {
            return nullptr;
        }

        const uintptr_t NextFreeAddress = *reinterpret_cast<uintptr_t*>(AvailableAddress);

        SlabToAllocate.UpdateFreeListHead(NextFreeAddress);

        ALLOCATOR_DIAGNOSTIC({ SlabToAllocate.IncrementActiveSlots(); });

        return reinterpret_cast<void*>(AvailableAddress);
        // ➤ Lock releases automatically here
    }

    [[nodiscard]] __attribute__((always_inline)) static inline bool
    CanFit(const SlabDescriptor& SlabToCheck) noexcept
    {
        return SlabToCheck.GetFreeListHead() != 0;
    }
};

} // namespace Allocator
