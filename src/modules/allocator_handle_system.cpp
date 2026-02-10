#include <modules/allocator_handle_system.h>

namespace Allocator {

HandleTableShard::HandleTableShard() noexcept
    : m_FreeListHead(g_FreeListEnd), m_ActiveCount(0), m_Capacity(0)
{
    for (auto& Page : m_Pages) {
        Page.store(nullptr, std::memory_order_relaxed);
    }
    GrowCapacity();
}

HandleTableShard::~HandleTableShard() noexcept
{
    for (auto& PageAtom : m_Pages) {
        HandleMetadata* Page = PageAtom.load(std::memory_order_relaxed);
        if (Page) {
            delete[] Page;
        }
    }
}

bool HandleTableShard::GrowCapacity() noexcept
{
    std::lock_guard<std::mutex> Lock(m_GrowthMutex);

    const uint32_t CurrentCap = m_Capacity.load(std::memory_order_relaxed);
    const uint32_t NextPageIndex = CurrentCap >> g_PageShift;

    if (NextPageIndex >= g_MaxPages) {
        return false;
    }

    HandleMetadata* NewPage = new (std::nothrow) HandleMetadata[g_ElementsPerPage];
    if (!NewPage) {
        return false;
    }

    const uint32_t BaseIndex = NextPageIndex << g_PageShift;

    for (uint32_t i = 0; i < g_ElementsPerPage - 1; ++i) {
        NewPage[i].NextFree = BaseIndex + i + 1;
        NewPage[i].Generation = 1;
    }

    uint32_t OldHead = m_FreeListHead.load(std::memory_order_relaxed);
    while (true) {
        NewPage[g_ElementsPerPage - 1].NextFree = OldHead;
        NewPage[g_ElementsPerPage - 1].Generation = 1;

        if (m_FreeListHead.compare_exchange_weak(OldHead, BaseIndex, std::memory_order_release,
                                                 std::memory_order_relaxed)) {
            break;
        }
    }

    m_Pages[NextPageIndex].store(NewPage, std::memory_order_release);
    m_Capacity.store(BaseIndex + g_ElementsPerPage, std::memory_order_release);
    return true;
}

HandleTableShard::ShardAllocResult HandleTableShard::AllocateRaw(void* Pointer) noexcept
{
    while (true) {
        uint32_t CurrentIndex = m_FreeListHead.load(std::memory_order_acquire);

        if (CurrentIndex == g_FreeListEnd) {
            if (!GrowCapacity()) {
                return {0, 0, false};
            }
            continue;
        }

        const uint32_t PageIndex = CurrentIndex >> g_PageShift;
        const uint32_t SlotIndex = CurrentIndex & g_PageMask;

        HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);
        if (!Page) {
            return {0, 0, false};
        }

        const uint32_t NextFree = Page[SlotIndex].NextFree;

        if (m_FreeListHead.compare_exchange_weak(CurrentIndex, NextFree, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            HandleMetadata& Meta = Page[SlotIndex];
            Meta.Pointer = Pointer;
            m_ActiveCount.fetch_add(1, std::memory_order_relaxed);
            return {CurrentIndex, Meta.Generation, true};
        }
    }
}

void* HandleTableShard::ResolveRaw(uint32_t Index, uint32_t Generation) const noexcept
{
    const uint32_t PageIndex = Index >> g_PageShift;

    if (PageIndex >= g_MaxPages)
        return nullptr;

    HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);
    if (!Page)
        return nullptr;

    const HandleMetadata& Meta = Page[Index & g_PageMask];
    if (Meta.Generation != Generation)
        return nullptr;

    return Meta.Pointer;
}

bool HandleTableShard::FreeRaw(uint32_t Index, uint32_t Generation) noexcept
{
    const uint32_t PageIndex = Index >> g_PageShift;
    const uint32_t SlotIndex = Index & g_PageMask;

    if (PageIndex >= g_MaxPages)
        return false;

    HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);
    if (!Page)
        return false;

    HandleMetadata& Meta = Page[SlotIndex];

    if (Meta.Generation != Generation) {
        return false;
    }

    Meta.Pointer = nullptr;

    uint32_t NextGen = Meta.Generation + 1;
    Meta.Generation = (NextGen == 0) ? 1 : NextGen;

    uint32_t CurrentHead = m_FreeListHead.load(std::memory_order_relaxed);
    do {
        Meta.NextFree = CurrentHead;
    } while (!m_FreeListHead.compare_exchange_weak(CurrentHead, Index, std::memory_order_release,
                                                   std::memory_order_relaxed));

    m_ActiveCount.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void HandleTableShard::ClearRaw() noexcept
{
    const uint32_t Cap = m_Capacity.load(std::memory_order_relaxed);
    const uint32_t ActivePages = Cap >> g_PageShift;

    for (uint32_t p = 0; p < ActivePages; ++p) {
        HandleMetadata* Page = m_Pages[p].load(std::memory_order_relaxed);
        if (!Page)
            continue;

        const uint32_t BaseIndex = p << g_PageShift;
        for (uint32_t i = 0; i < g_ElementsPerPage; ++i) {
            Page[i].Pointer = nullptr;
            Page[i].Generation++;
            if (Page[i].Generation == 0)
                Page[i].Generation = 1;
            Page[i].NextFree = BaseIndex + i + 1;
        }

        if (p == ActivePages - 1) {
            Page[g_ElementsPerPage - 1].NextFree = g_FreeListEnd;
        }
    }

    if (ActivePages > 0) {
        m_FreeListHead.store(0, std::memory_order_release);
    }
    else {
        m_FreeListHead.store(g_FreeListEnd, std::memory_order_release);
    }

    m_ActiveCount.store(0, std::memory_order_relaxed);
}

HandleTable::HandleTable(uint32_t) noexcept {}

Handle HandleTable::Allocate(void* Pointer) noexcept
{
    if (!Pointer)
        return g_InvalidHandle;

    const size_t ShardID = GetShardIndex();
    auto Result = m_Shards[ShardID].AllocateRaw(Pointer);

    if (Result.Success) {
        return Handle(Result.Index, Result.Generation, static_cast<uint8_t>(ShardID));
    }

    return g_InvalidHandle;
}

void* HandleTable::Resolve(Handle H) const noexcept
{
    if (!H.IsValid())
        return nullptr;
    return m_Shards[H.GetShardID()].ResolveRaw(H.GetIndex(), H.GetGeneration());
}

bool HandleTable::Free(Handle H) noexcept
{
    if (!H.IsValid())
        return false;
    return m_Shards[H.GetShardID()].FreeRaw(H.GetIndex(), H.GetGeneration());
}

void HandleTable::Clear() noexcept
{
    for (auto& Shard : m_Shards) {
        Shard.ClearRaw();
    }
}

uint32_t HandleTable::GetActiveCount() const noexcept
{
    uint32_t Total = 0;
    for (const auto& Shard : m_Shards) {
        Total += Shard.GetActiveCount();
    }
    return Total;
}

uint32_t HandleTable::GetCapacity() const noexcept
{
    return 0;
}

} // namespace Allocator
