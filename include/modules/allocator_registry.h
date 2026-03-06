#pragma once
#include <utilities/allocator_utility.h>

namespace Allocator {

const size_t g_ConstSlabSize = 64 * 1024;
const size_t g_ConstArenaSize = 64 * 1024 * 1024;

static constexpr size_t g_BitsPerBlock = 64;
static constexpr uint64_t g_EmptyBlock = 0ULL;
static constexpr uint64_t g_FullBlock = 0xFFFFFFFFFFFFFFFFULL;

struct SlabConfig
{
    uintptr_t p_StartAddress;
    uintptr_t p_FreeListHead;
    size_t p_TotalSlots;
    size_t p_SlabMemory;
};

class SlabDescriptor
{
private:
    uintptr_t m_SlabStart;
    std::atomic<uintptr_t> m_FreeListHead;
    SlabDescriptor* m_NextSlab;
    size_t m_ActiveSlots;
    size_t m_TotalSlots;
    size_t m_AvailableSlabMemory;

    mutable std::mutex m_SlabMutex;

public:
    SlabDescriptor(const SlabConfig& Config) noexcept;

    void ResetSlab() noexcept;

    std::mutex& GetMutex() const noexcept { return m_SlabMutex; }

    inline void UpdateFreeListHead(uintptr_t FreeListHead) noexcept
    {
        m_FreeListHead.store(FreeListHead, std::memory_order_relaxed);
    }
    inline void IncrementActiveSlots() noexcept { m_ActiveSlots++; }
    inline void SetActiveSlots(size_t ActiveSlots) noexcept { m_ActiveSlots = ActiveSlots; }
    inline void SetNextSlab(SlabDescriptor* NextSlab) noexcept { m_NextSlab = NextSlab; }
    inline void SetTotalSlots(size_t TotalSlots) noexcept { m_TotalSlots = TotalSlots; }

    [[nodiscard]] inline uintptr_t GetSlabStart() const noexcept { return m_SlabStart; }
    [[nodiscard]] inline uintptr_t GetFreeListHead() const noexcept
    {
        return m_FreeListHead.load(std::memory_order_relaxed);
    }
    [[nodiscard]] inline std::atomic<uintptr_t>& GetFreeListHeadAtomic() noexcept
    {
        return m_FreeListHead;
    }
    [[nodiscard]] inline size_t GetActiveSlots() const noexcept { return m_ActiveSlots; }
    [[nodiscard]] inline size_t GetTotalSlots() const noexcept { return m_TotalSlots; }
    [[nodiscard]] inline size_t GetAvailableMemorySize() const noexcept
    {
        return m_AvailableSlabMemory;
    }
    [[nodiscard]] inline SlabDescriptor* GetNextSlab() const noexcept { return m_NextSlab; }
};

class SlabRegistry
{
private:
    std::span<SlabDescriptor> m_DescriptorSpan;
    size_t m_DescriptorCount;

    std::unique_ptr<std::atomic<uint64_t>[]> m_BitMap;
    std::unique_ptr<std::atomic<uint64_t>[]> m_SuperBlock;
    size_t m_BitMapSizeInWords;
    size_t m_SuperBlockSize;

    std::atomic<size_t> m_SearchHint{0};

    void* m_ArenaRegistryStart;
    void* m_ArenaSlabsStart;

    size_t m_ArenaSize;
    size_t m_SlabSize;

    [[nodiscard]] bool InitializeArena() noexcept;
    void ShutdownArena() noexcept;

public:
    SlabRegistry(size_t SlabSize = g_ConstSlabSize,
                 size_t RequestedArenaSize = g_ConstArenaSize) noexcept;

    ~SlabRegistry() noexcept;

    SlabRegistry(const SlabRegistry&) = delete;
    SlabRegistry& operator=(const SlabRegistry&) = delete;

    [[nodiscard]] SlabDescriptor* AllocateSlab() noexcept;
    size_t AllocateSlabBatch(size_t RequestCount, SlabDescriptor** OutSlabs) noexcept;
    void FreeSlab(SlabDescriptor* SlabToFree) noexcept;

    [[nodiscard]] SlabDescriptor* GetSlabDescriptor(void* Ptr) const noexcept;

    [[nodiscard]] inline size_t GetSlabSize() const noexcept { return m_SlabSize; }
    [[nodiscard]] inline size_t GetArenaSize() const noexcept { return m_ArenaSize; }
    [[nodiscard]] inline void* GetArenaSlabsStart() const noexcept { return m_ArenaSlabsStart; }
};
} // namespace Allocator
