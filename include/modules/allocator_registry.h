#pragma once
#include <utilities/allocator_utility.h>

namespace Allocator {

const size_t g_ConstSlabSize = static_cast<size_t>(64 * 1024);
const size_t g_ConstArenaSize = static_cast<size_t>(64 * 1024 * 1024);

static constexpr size_t g_BitsPerBlock = 64;
static constexpr uint64_t g_EmptyBlock = 0ULL;
static constexpr uint64_t g_FullBlock = 0xFFFFFFFFFFFFFFFFULL;
static constexpr size_t g_AlignmentMask = g_BitsPerBlock - 1;

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
    uintptr_t m_FreeListHead;
    size_t m_ActiveSlots;
    size_t m_TotalSlots;
    SlabDescriptor* m_NextSlab;
    size_t m_AvailableSlabMemory;

public:
    SlabDescriptor(const SlabConfig& Config) noexcept;

    void ResetSlab() noexcept;

    void SetSlabStart(uintptr_t StartAddress) noexcept;
    void SetTotalSlots(size_t TotalSlots) noexcept;
    void SetActiveSlots(size_t ActiveSlots) noexcept;
    void IncrementActiveSlots() noexcept;
    void SetNextSlab(SlabDescriptor* NextSlab) noexcept;
    void UpdateFreeListHead(uintptr_t FreeListHead) noexcept;

    [[nodiscard]] uintptr_t GetSlabStart() const noexcept;
    [[nodiscard]] uintptr_t GetFreeListHead() const noexcept;
    [[nodiscard]] size_t GetTotalSlots() const noexcept;
    [[nodiscard]] size_t GetActiveSlots() const noexcept;
    [[nodiscard]] size_t GetAvailableMemorySize() const noexcept;
    [[nodiscard]] SlabDescriptor* GetNextSlab() const noexcept;
};

class SlabRegistry
{
private:
    std::span<SlabDescriptor> m_DescriptorSpan;
    size_t m_DescriptorCount;

    std::unique_ptr<std::atomic<uint64_t>[]> m_BitMap;
    std::atomic<size_t> m_SearchHint{0};
    size_t m_BitMapSizeInWords;

    void* m_ArenaRegistryStart;
    void* m_ArenaSlabsStart;

    size_t m_ArenaSize;
    size_t m_SlabSize;

    std::mutex m_AllocationMutex;

    [[nodiscard]] bool InitializeArena() noexcept;
    void ShutdownArena() noexcept;

public:
    SlabRegistry(size_t SlabSize = g_ConstSlabSize,
                 size_t RequestedArenaSize = g_ConstArenaSize) noexcept;

    ~SlabRegistry() noexcept;
    SlabRegistry(const SlabRegistry&) = delete;
    SlabRegistry& operator=(const SlabRegistry&) = delete;
    SlabRegistry(SlabRegistry&&) = delete;
    SlabRegistry& operator=(SlabRegistry&&) = delete;

    [[nodiscard]] size_t GetDescriptorCount() const noexcept;
    [[nodiscard]] void* GetArenaSlabsStart() const noexcept;

    [[nodiscard]] size_t GetSlabSize() const noexcept;
    [[nodiscard]] size_t GetArenaSize() const noexcept;

    [[nodiscard]] SlabDescriptor* AllocateSlab() noexcept;
    void FreeSlab(SlabDescriptor* SlabToFree) noexcept;
};
} // namespace Allocator
