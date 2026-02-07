#include <Core/core.h>
#include <modules/allocator_handle_system.h>

namespace Allocator {

HandleTable::HandleTable(uint32_t InitialCapacity) noexcept
    : m_FreeListHead(g_FreeListEnd), m_Capacity(0), m_ActiveCount(0)
{
    for (auto& Page : m_Pages) {
        Page.store(nullptr, std::memory_order_relaxed);
    }

    uint32_t SafeInitial = (InitialCapacity == 0) ? g_ElementsPerPage : InitialCapacity;
    uint32_t PagesNeeded = (SafeInitial + g_ElementsPerPage - 1) / g_ElementsPerPage;

    for (uint32_t i = 0; i < PagesNeeded; ++i) {
        GrowCapacity();
    }
}

HandleTable::~HandleTable() noexcept
{
    LOG_ALLOCATOR("INFO", "HandleTable: Shutting down.");
    for (auto& PageAtom : m_Pages) {
        HandleMetadata* Page = PageAtom.load(std::memory_order_relaxed);
        if (Page) {
            delete[] Page;
        }
    }
}

bool HandleTable::GrowCapacity() noexcept
{
    std::lock_guard<std::mutex> Lock(m_GrowthMutex);

    const uint32_t CurrentCap = m_Capacity.load(std::memory_order_relaxed);
    const uint32_t NextPageIndex = CurrentCap >> g_PageShift;

    if (NextPageIndex >= g_MaxPages) [[unlikely]] {
        LOG_ALLOCATOR("CRITICAL", "HandleTable: Max Capacity Reached.");
        return false;
    }

    HandleMetadata* NewPage = new (std::nothrow) HandleMetadata[g_ElementsPerPage];
    if (!NewPage) [[unlikely]]
        return false;

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

Handle HandleTable::Allocate(void* Pointer) noexcept
{
    ALLOCATOR_ASSERT(Pointer != nullptr, "HandleTable: Nullptr allocation attempted.");

    while (true) {
        uint32_t CurrentIndex = m_FreeListHead.load(std::memory_order_acquire);

        if (CurrentIndex == g_FreeListEnd) [[unlikely]] {
            if (!GrowCapacity())
                return g_InvalidHandle;
            continue;
        }

        const uint32_t PageIndex = CurrentIndex >> g_PageShift;
        const uint32_t SlotIndex = CurrentIndex & g_PageMask;

        HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);
        const uint32_t NextFree = Page[SlotIndex].NextFree;

        if (m_FreeListHead.compare_exchange_weak(CurrentIndex, NextFree, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            HandleMetadata& Meta = Page[SlotIndex];
            Meta.Pointer = Pointer;

            m_ActiveCount.fetch_add(1, std::memory_order_relaxed);

            return Handle(CurrentIndex, Meta.Generation);
        }
    }
}

void* HandleTable::Resolve(Handle H) const noexcept
{
    if (!H.IsValid()) [[unlikely]]
        return nullptr;

    const uint32_t Index = H.GetIndex();
    const uint32_t PageIndex = Index >> g_PageShift;

    HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);
    if (!Page) [[unlikely]]
        return nullptr;

    const HandleMetadata& Meta = Page[Index & g_PageMask];

    return (Meta.Generation == H.GetGeneration()) ? Meta.Pointer : nullptr;
}

bool HandleTable::Free(Handle H) noexcept
{
    if (!H.IsValid()) [[unlikely]]
        return false;

    const uint32_t Index = H.GetIndex();
    const uint32_t PageIndex = Index >> g_PageShift;
    const uint32_t SlotIndex = Index & g_PageMask;

    HandleMetadata* Page = m_Pages[PageIndex].load(std::memory_order_acquire);
    HandleMetadata& Meta = Page[SlotIndex];

    if (Meta.Generation != H.GetGeneration()) [[unlikely]]
        return false;

    Meta.Pointer = nullptr;

    Meta.Generation = (Meta.Generation + 1) == 0 ? 1 : Meta.Generation + 1;

    uint32_t CurrentHead = m_FreeListHead.load(std::memory_order_relaxed);
    do {
        Meta.NextFree = CurrentHead;
    } while (!m_FreeListHead.compare_exchange_weak(CurrentHead, Index, std::memory_order_release,
                                                   std::memory_order_relaxed));

    m_ActiveCount.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void HandleTable::Clear() noexcept
{
    LOG_ALLOCATOR("INFO", "HandleTable: Performing high-speed bulk clear.");

    const uint32_t Cap = m_Capacity.load(std::memory_order_relaxed);
    const uint32_t ActivePages = Cap >> g_PageShift;

    for (uint32_t p = 0; p < ActivePages; ++p) {
        HandleMetadata* Page = m_Pages[p].load(std::memory_order_relaxed);
        if (!Page) [[unlikely]]
            continue;

        const uint32_t BaseIndex = p << g_PageShift;

        for (uint32_t i = 0; i < g_ElementsPerPage; ++i) {
            Page[i].Pointer = nullptr;

            uint32_t NextGen = Page[i].Generation + 1;
            Page[i].Generation = (NextGen == 0) ? 1 : NextGen;

            Page[i].NextFree = BaseIndex + i + 1;
        }
    }

    if (ActivePages > 0) {
        HandleMetadata* LastPage = m_Pages[ActivePages - 1].load(std::memory_order_relaxed);
        LastPage[g_ElementsPerPage - 1].NextFree = g_FreeListEnd;
    }

    m_ActiveCount.store(0, std::memory_order_relaxed);
}

} // namespace Allocator
