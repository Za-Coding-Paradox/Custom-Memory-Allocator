#include <modules/strategies/pool_module/pool_scopes.h>

namespace Allocator {

class PoolStrategy
{
public:
    PoolStrategy() = delete;
    ~PoolStrategy() = delete;

    static void Format(SlabDescriptor& SlabToInitialize, size_t ChunkSize) noexcept;

    [[nodiscard]] static void* Allocate(SlabDescriptor& SlabToAllocate) noexcept;

    static void Free(SlabDescriptor& SlabToFree, void* MemoryToFree) noexcept;

    [[nodiscard]] static bool CanFit(const SlabDescriptor& SlabToCheck) noexcept;
};

} // namespace Allocator
