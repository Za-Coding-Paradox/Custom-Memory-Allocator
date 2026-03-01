#pragma once

#include <utilities/allocator_utility.h>

namespace Allocator {

class Handle
{
private:
    uint64_t m_Packed;

    static constexpr uint64_t SHARD_BITS = 5;
    static constexpr uint64_t INDEX_BITS = 27;
    static constexpr uint64_t GEN_BITS = 32;

    static constexpr uint64_t SHARD_MASK = (1ULL << SHARD_BITS) - 1;
    static constexpr uint64_t INDEX_MASK = (1ULL << INDEX_BITS) - 1;

public:
    constexpr Handle() noexcept : m_Packed(0) {}

    constexpr Handle(uint32_t Index, uint32_t Generation, uint8_t ShardID) noexcept
        : m_Packed((static_cast<uint64_t>(Generation) << (SHARD_BITS + INDEX_BITS)) |
                   (static_cast<uint64_t>(ShardID & SHARD_MASK) << INDEX_BITS) |
                   (Index & INDEX_MASK))
    {}

    [[nodiscard]] uint32_t GetIndex() const noexcept
    {
        return static_cast<uint32_t>(m_Packed & INDEX_MASK);
    }

    [[nodiscard]] uint32_t GetGeneration() const noexcept
    {
        return static_cast<uint32_t>(m_Packed >> (SHARD_BITS + INDEX_BITS));
    }

    [[nodiscard]] uint8_t GetShardID() const noexcept
    {
        return static_cast<uint8_t>((m_Packed >> INDEX_BITS) & SHARD_MASK);
    }

    [[nodiscard]] bool IsValid() const noexcept { return m_Packed != 0; }

    bool operator==(const Handle& Other) const noexcept { return m_Packed == Other.m_Packed; }
    bool operator!=(const Handle& Other) const noexcept { return m_Packed != Other.m_Packed; }
};

inline constexpr Handle g_InvalidHandle = Handle();

struct HandleMetadata
{
    void* Pointer;
    uint32_t Generation;
    uint32_t NextFree;

    HandleMetadata() noexcept : Pointer(nullptr), Generation(1), NextFree(0) {}
};

class alignas(64) HandleTableShard
{
    friend class HandleTable;

private:
    static constexpr uint32_t g_ElementsPerPage = 1024;
    static constexpr uint32_t g_MaxPages = 65536;
    static constexpr uint32_t g_FreeListEnd = 0xFFFFFFFF;
    static constexpr uint32_t g_PageShift = 10;
    static constexpr uint32_t g_PageMask = g_ElementsPerPage - 1;

    std::array<std::atomic<HandleMetadata*>, g_MaxPages> m_Pages;

    alignas(64) std::atomic<uint64_t> m_TaggedHead;
    alignas(64) std::atomic<uint32_t> m_ActiveCount;
    alignas(64) std::atomic<uint32_t> m_Capacity;

    mutable std::mutex m_GrowthMutex;

    struct ShardAllocResult
    {
        uint32_t Index;
        uint32_t Generation;
        bool Success;
    };

    bool GrowCapacity() noexcept;

    static constexpr uint64_t PackHead(uint32_t Index, uint32_t Tag) noexcept
    {
        return (static_cast<uint64_t>(Tag) << 32) | static_cast<uint64_t>(Index);
    }
    static constexpr uint32_t HeadIndex(uint64_t Packed) noexcept
    {
        return static_cast<uint32_t>(Packed);
    }
    static constexpr uint32_t HeadTag(uint64_t Packed) noexcept
    {
        return static_cast<uint32_t>(Packed >> 32);
    }

public:
    explicit HandleTableShard() noexcept;
    ~HandleTableShard() noexcept;

    [[nodiscard]] ShardAllocResult AllocateRaw(void* Pointer) noexcept;
    [[nodiscard]] void* ResolveRaw(uint32_t Index, uint32_t Generation) const noexcept;
    bool FreeRaw(uint32_t Index, uint32_t Generation) noexcept;
    void ClearRaw() noexcept;

    [[nodiscard]] uint32_t GetActiveCount() const noexcept
    {
        return m_ActiveCount.load(std::memory_order_relaxed);
    }
};

class HandleTable
{
private:
    static constexpr size_t NUM_SHARDS = 32;

    std::array<HandleTableShard, NUM_SHARDS> m_Shards;

    struct alignas(64) ShardCache
    {
        size_t Index;
    };

    [[nodiscard]] __attribute__((always_inline)) inline size_t GetShardIndex() const noexcept
    {
        static thread_local const ShardCache Cache = {
            std::hash<std::thread::id>{}(std::this_thread::get_id()) % NUM_SHARDS};
        return Cache.Index;
    }

public:
    HandleTable(uint32_t InitialCapacity = 0) noexcept;
    ~HandleTable() noexcept = default;

    [[nodiscard]] Handle Allocate(void* Pointer) noexcept;
    [[nodiscard]] void* Resolve(Handle H) const noexcept;
    bool Free(Handle H) noexcept;
    void Clear() noexcept;

    [[nodiscard]] uint32_t GetActiveCount() const noexcept;
    [[nodiscard]] uint32_t GetCapacity() const noexcept;
};

} // namespace Allocator
