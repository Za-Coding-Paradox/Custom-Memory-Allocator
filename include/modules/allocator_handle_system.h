#pragma once

#include <utilities/allocator_utility.h>

namespace Allocator {

class Handle
{
private:
    uint64_t m_Packed;

public:
    constexpr Handle() noexcept : m_Packed(0) {}
    constexpr Handle(uint32_t Index, uint32_t Generation) noexcept
        : m_Packed((static_cast<uint64_t>(Generation) << 32) | Index)
    {}

    [[nodiscard]] uint32_t GetIndex() const noexcept
    {
        return static_cast<uint32_t>(m_Packed & 0xFFFFFFFF);
    }
    [[nodiscard]] uint32_t GetGeneration() const noexcept
    {
        return static_cast<uint32_t>(m_Packed >> 32);
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

class HandleTable
{
private:
    static constexpr uint32_t g_ElementsPerPage = 1024;
    static constexpr uint32_t g_MaxPages = 1024;
    static constexpr uint32_t g_FreeListEnd = 0xFFFFFFFF;
    static constexpr uint32_t g_PageShift = 10;
    static constexpr uint32_t g_PageMask = g_ElementsPerPage - 1;

    std::array<std::atomic<HandleMetadata*>, g_MaxPages> m_Pages;
    std::atomic<uint32_t> m_FreeListHead;
    std::atomic<uint32_t> m_Capacity;
    std::atomic<uint32_t> m_ActiveCount;
    mutable std::mutex m_GrowthMutex;

    bool GrowCapacity() noexcept;

public:
    explicit HandleTable(uint32_t InitialCapacity = g_ElementsPerPage) noexcept;
    ~HandleTable() noexcept;

    [[nodiscard]] Handle Allocate(void* Pointer) noexcept;
    [[nodiscard]] void* Resolve(Handle H) const noexcept;
    bool Free(Handle H) noexcept;

    [[nodiscard]] uint32_t GetActiveCount() const noexcept
    {
        return m_ActiveCount.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint32_t GetCapacity() const noexcept
    {
        return m_Capacity.load(std::memory_order_relaxed);
    }

    void Clear() noexcept;
};

} // namespace Allocator
