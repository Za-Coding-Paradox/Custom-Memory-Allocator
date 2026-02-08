#include <modules/allocator_registry.h>

namespace Allocator {

SlabDescriptor::SlabDescriptor(const SlabConfig& Config) noexcept
    : m_SlabStart(Config.p_StartAddress), m_FreeListHead(Config.p_FreeListHead),
      m_NextSlab(nullptr), m_ActiveSlots(0), m_TotalSlots(Config.p_TotalSlots),
      m_AvailableSlabMemory(Config.p_SlabMemory)
{}

void SlabDescriptor::ResetSlab() noexcept
{
    std::lock_guard<std::mutex> Lock(m_SlabMutex);
    m_FreeListHead = m_SlabStart;
    m_ActiveSlots = 0;
    m_NextSlab = nullptr;
}

SlabRegistry::SlabRegistry(size_t SlabSize, size_t RequestedArenaSize) noexcept
    : m_DescriptorCount(0), m_BitMapSizeInWords(0), m_SuperBlockSize(0),
      m_ArenaRegistryStart(nullptr), m_ArenaSlabsStart(nullptr), m_ArenaSize(RequestedArenaSize),
      m_SlabSize(SlabSize)
{
    LOG_ALLOCATOR("INFO", "SlabRegistry: Initializing Arena. Size: " << m_ArenaSize);
    if (!InitializeArena()) [[unlikely]] {
        LOG_ALLOCATOR("CRITICAL", "SlabRegistry: Arena Initialization Failed!");
    }
}

SlabRegistry::~SlabRegistry() noexcept
{
    ShutdownArena();
}

bool SlabRegistry::InitializeArena() noexcept
{
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

    int Flags = MAP_PRIVATE | MAP_ANONYMOUS;

    m_ArenaRegistryStart =
        mmap(nullptr, m_ArenaSize, PROT_READ | PROT_WRITE, Flags | MAP_HUGETLB, -1, 0);

    if (m_ArenaRegistryStart == MAP_FAILED) {
        LOG_ALLOCATOR("WARN", "SlabRegistry: Huge Pages not supported. Falling back to 4KB pages.");

        m_ArenaRegistryStart = mmap(nullptr, m_ArenaSize, PROT_READ | PROT_WRITE, Flags, -1, 0);
    }
    else {
        LOG_ALLOCATOR("INFO", "SlabRegistry: Huge Pages Enabled (TLB Optimization Active).");
    }

    if (m_ArenaRegistryStart == MAP_FAILED) {
        LOG_ALLOCATOR("CRITICAL",
                      "SlabRegistry: mmap failed completely. Error: " << strerror(errno));
        m_ArenaRegistryStart = nullptr;
        return false;
    }

    std::memset(m_ArenaRegistryStart, 0, m_ArenaSize);
    LOG_ALLOCATOR("DEBUG", "SlabRegistry: mmap success at " << m_ArenaRegistryStart);

    size_t UnitSize = sizeof(SlabDescriptor) + m_SlabSize;
    m_DescriptorCount = m_ArenaSize / UnitSize;

    if (m_DescriptorCount == 0) {
        LOG_ALLOCATOR("CRITICAL", "SlabRegistry: Arena size too small for even one slab.");
        return false;
    }

    m_BitMapSizeInWords = (m_DescriptorCount + 63) / 64;
    m_BitMap = std::make_unique<std::atomic<uint64_t>[]>(m_BitMapSizeInWords);
    for (size_t i = 0; i < m_BitMapSizeInWords; ++i)
        m_BitMap[i].store(0);

    m_SuperBlockSize = (m_BitMapSizeInWords + 63) / 64;
    m_SuperBlock = std::make_unique<std::atomic<uint64_t>[]>(m_SuperBlockSize);
    for (size_t i = 0; i < m_SuperBlockSize; ++i)
        m_SuperBlock[i].store(0);

    auto* DescriptorBase = static_cast<SlabDescriptor*>(m_ArenaRegistryStart);

    uintptr_t SlabRegionStart = reinterpret_cast<uintptr_t>(m_ArenaRegistryStart) +
                                (m_DescriptorCount * sizeof(SlabDescriptor));

    SlabRegionStart = (SlabRegionStart + 4095) & ~static_cast<uintptr_t>(4095);
    m_ArenaSlabsStart = reinterpret_cast<void*>(SlabRegionStart);

    uint8_t* CurrentSlabPtr = static_cast<uint8_t*>(m_ArenaSlabsStart);
    uintptr_t ArenaEnd = reinterpret_cast<uintptr_t>(m_ArenaRegistryStart) + m_ArenaSize;

    LOG_ALLOCATOR("DEBUG", "SlabRegistry: Creating " << m_DescriptorCount << " descriptors.");

    for (size_t i = 0; i < m_DescriptorCount; ++i) {
        if (reinterpret_cast<uintptr_t>(CurrentSlabPtr + m_SlabSize) > ArenaEnd) {
            LOG_ALLOCATOR("WARN", "SlabRegistry: Arena boundary reached at index " << i);
            m_DescriptorCount = i;
            break;
        }

        SlabConfig Config{.p_StartAddress = reinterpret_cast<uintptr_t>(CurrentSlabPtr),
                          .p_FreeListHead = reinterpret_cast<uintptr_t>(CurrentSlabPtr),
                          .p_TotalSlots = 0,
                          .p_SlabMemory = m_SlabSize};

        new (&DescriptorBase[i]) SlabDescriptor(Config);

        CurrentSlabPtr += m_SlabSize;
    }

    m_DescriptorSpan = std::span<SlabDescriptor>(DescriptorBase, m_DescriptorCount);
    LOG_ALLOCATOR("INFO", "SlabRegistry: Ready. Total Slabs: " << m_DescriptorCount);
    return true;
}

SlabDescriptor* SlabRegistry::AllocateSlab() noexcept
{
    for (size_t sbIdx = 0; sbIdx < m_SuperBlockSize; ++sbIdx) {

        uint64_t SuperWord = m_SuperBlock[sbIdx].load(std::memory_order_relaxed);
        if (SuperWord == g_FullBlock)
            continue;

        size_t BaseWordIdx = sbIdx * 64;

        for (size_t i = 0; i < 64; ++i) {
            size_t WordIdx = BaseWordIdx + i;
            if (WordIdx >= m_BitMapSizeInWords)
                break;

            uint64_t Word = m_BitMap[WordIdx].load(std::memory_order_relaxed);
            if (Word == g_FullBlock)
                continue;

            const int BitIdx = std::countr_one(Word);
            if (BitIdx >= 64)
                continue;

            const uint64_t Mask = 1ULL << BitIdx;

            if (!(Word & Mask)) {
                if (m_BitMap[WordIdx].compare_exchange_weak(
                        Word, Word | Mask, std::memory_order_acquire, std::memory_order_relaxed)) {

                    const size_t GlobalIdx = (WordIdx << 6) | static_cast<size_t>(BitIdx);
                    if (GlobalIdx >= m_DescriptorCount)
                        return nullptr;

                    if ((Word | Mask) == g_FullBlock) {
                        const uint64_t SuperMask = 1ULL << i;
                        m_SuperBlock[sbIdx].fetch_or(SuperMask, std::memory_order_relaxed);
                    }

                    SlabDescriptor* Slab = &m_DescriptorSpan[GlobalIdx];
                    Slab->ResetSlab();

                    LOG_ALLOCATOR("DEBUG", "SlabRegistry: Allocated Slab Index " << GlobalIdx);
                    return Slab;
                }
            }
        }
    }

    LOG_ALLOCATOR("ERROR", "SlabRegistry: OUT OF MEMORY (Arena Full)");
    return nullptr;
}

void SlabRegistry::FreeSlab(SlabDescriptor* SlabToFree) noexcept
{
    if (!SlabToFree)
        return;

    const ptrdiff_t Idx = SlabToFree - m_DescriptorSpan.data();
    if (Idx < 0 || static_cast<size_t>(Idx) >= m_DescriptorCount) {
        LOG_ALLOCATOR("ERROR", "SlabRegistry: Attempted to free out-of-bounds slab pointer!");
        return;
    }

    const size_t WordIdx = static_cast<size_t>(Idx) >> 6;
    const uint64_t Mask = 1ULL << (static_cast<size_t>(Idx) & 63);

    uint64_t OldVal = m_BitMap[WordIdx].fetch_and(~Mask, std::memory_order_release);

    if (OldVal == g_FullBlock) {
        const size_t SbIdx = WordIdx / 64;
        const size_t SbBit = WordIdx % 64;
        m_SuperBlock[SbIdx].fetch_and(~(1ULL << SbBit), std::memory_order_relaxed);
    }

    LOG_ALLOCATOR("DEBUG", "SlabRegistry: Freed Slab Index " << Idx);
}

SlabDescriptor* SlabRegistry::GetSlabDescriptor(void* Ptr) const noexcept
{
    if (!Ptr || !m_ArenaSlabsStart)
        return nullptr;

    const uintptr_t PtrVal = reinterpret_cast<uintptr_t>(Ptr);
    const uintptr_t StartVal = reinterpret_cast<uintptr_t>(m_ArenaSlabsStart);

    if (PtrVal < StartVal || PtrVal >= (StartVal + (m_DescriptorCount * m_SlabSize))) {
        return nullptr;
    }

    const size_t Offset = PtrVal - StartVal;
    const size_t Index = Offset / m_SlabSize;

    return &m_DescriptorSpan[Index];
}

void SlabRegistry::ShutdownArena() noexcept
{
    if (m_ArenaRegistryStart) {
        LOG_ALLOCATOR("INFO", "SlabRegistry: Unmapping Arena at " << m_ArenaRegistryStart);
        munmap(m_ArenaRegistryStart, m_ArenaSize);
        m_ArenaRegistryStart = nullptr;
    }
}

} // namespace Allocator
