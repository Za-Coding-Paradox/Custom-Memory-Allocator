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

// allocator_registry.cpp

bool SlabRegistry::InitializeArena() noexcept
{
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

    int Flags = MAP_PRIVATE | MAP_ANONYMOUS;

    m_ArenaRegistryStart =
        mmap(nullptr, m_ArenaSize, PROT_READ | PROT_WRITE, Flags | MAP_HUGETLB, -1, 0);

    if (m_ArenaRegistryStart == MAP_FAILED) {
        LOG_ALLOCATOR("WARN", "SlabRegistry: Huge Pages unavailable. Falling back to 4KB pages.");
        m_ArenaRegistryStart = mmap(nullptr, m_ArenaSize, PROT_READ | PROT_WRITE, Flags, -1, 0);
    }
    else {
        LOG_ALLOCATOR("INFO", "SlabRegistry: Huge Pages Enabled (TLB Optimization Active).");
    }

    if (m_ArenaRegistryStart == MAP_FAILED) {
        LOG_ALLOCATOR("CRITICAL", "SlabRegistry: mmap failed. Error: " << strerror(errno));
        m_ArenaRegistryStart = nullptr;
        return false;
    }

    const uintptr_t ArenaBase = reinterpret_cast<uintptr_t>(m_ArenaRegistryStart);
    const uintptr_t ArenaEnd = ArenaBase + m_ArenaSize;

    auto PageAlign = [](uintptr_t Addr) -> uintptr_t {
        return (Addr + 4095ULL) & ~static_cast<uintptr_t>(4095);
    };

    auto ComputeSlabRegionStart = [&](size_t N) -> uintptr_t {
        return PageAlign(ArenaBase + N * sizeof(SlabDescriptor));
    };

    auto FitsInArena = [&](size_t N) -> bool {
        if (N == 0)
            return false;
        const uintptr_t SlabStart = ComputeSlabRegionStart(N);
        const uintptr_t SlabEnd = SlabStart + N * m_SlabSize;
        return SlabEnd <= ArenaEnd;
    };

    // Conservative upper bound
    size_t N = m_ArenaSize / (sizeof(SlabDescriptor) + m_SlabSize);

    if (N == 0) {
        LOG_ALLOCATOR("CRITICAL", "SlabRegistry: Arena too small for even one slab.");
        munmap(m_ArenaRegistryStart, m_ArenaSize);
        m_ArenaRegistryStart = nullptr;
        return false;
    }

    // Trim until actual layout fits — in practice this loops 0 or 1 times
    while (N > 0 && !FitsInArena(N))
        --N;

    if (N == 0) {
        LOG_ALLOCATOR("CRITICAL", "SlabRegistry: No slabs fit after alignment accounting.");
        munmap(m_ArenaRegistryStart, m_ArenaSize);
        m_ArenaRegistryStart = nullptr;
        return false;
    }

    m_DescriptorCount = N;

    std::memset(m_ArenaRegistryStart, 0, m_ArenaSize);

    m_BitMapSizeInWords = (m_DescriptorCount + 63) / 64;
    m_BitMap = std::make_unique<std::atomic<uint64_t>[]>(m_BitMapSizeInWords);
    for (size_t i = 0; i < m_BitMapSizeInWords; ++i)
        m_BitMap[i].store(g_EmptyBlock, std::memory_order_relaxed);

    m_SuperBlockSize = (m_BitMapSizeInWords + 63) / 64;
    m_SuperBlock = std::make_unique<std::atomic<uint64_t>[]>(m_SuperBlockSize);
    for (size_t i = 0; i < m_SuperBlockSize; ++i)
        m_SuperBlock[i].store(g_EmptyBlock, std::memory_order_relaxed);

    auto* DescriptorBase = static_cast<SlabDescriptor*>(m_ArenaRegistryStart);
    const uintptr_t SlabRegionStart = ComputeSlabRegionStart(m_DescriptorCount);
    m_ArenaSlabsStart = reinterpret_cast<void*>(SlabRegionStart);

    LOG_ALLOCATOR("DEBUG",
                  "SlabRegistry: N="
                      << m_DescriptorCount << " DescRegion=[0x" << std::hex << ArenaBase << "..0x"
                      << (ArenaBase + m_DescriptorCount * sizeof(SlabDescriptor)) << "]"
                      << " SlabRegion=[0x" << SlabRegionStart << "..0x"
                      << (SlabRegionStart + m_DescriptorCount * m_SlabSize) << "]" << std::dec);

    uint8_t* CurrentSlabPtr = static_cast<uint8_t*>(m_ArenaSlabsStart);

    for (size_t i = 0; i < m_DescriptorCount; ++i) {
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

        const size_t BaseWordIdx = sbIdx * g_BitsPerBlock;

        for (size_t i = 0; i < g_BitsPerBlock; ++i) {
            const size_t WordIdx = BaseWordIdx + i;
            if (WordIdx >= m_BitMapSizeInWords)
                break;

            uint64_t Word = m_BitMap[WordIdx].load(std::memory_order_relaxed);

            if (Word == g_FullBlock)
                continue;

            while (Word != g_FullBlock) {
                const int BitIdx = std::countr_one(Word);
                if (BitIdx >= 64)
                    break;

                const uint64_t Mask = 1ULL << static_cast<uint64_t>(BitIdx);
                const uint64_t NewWord = Word | Mask;

                if (m_BitMap[WordIdx].compare_exchange_weak(
                        Word, NewWord, std::memory_order_acquire, std::memory_order_relaxed)) {
                    const size_t GlobalIdx = (WordIdx << 6) | static_cast<size_t>(BitIdx);

                    if (GlobalIdx >= m_DescriptorCount) [[unlikely]] {
                        m_BitMap[WordIdx].fetch_and(~Mask, std::memory_order_release);
                        LOG_ALLOCATOR("ERROR", "SlabRegistry: Claimed bit "
                                                   << GlobalIdx << " exceeds descriptor count "
                                                   << m_DescriptorCount);
                        return nullptr;
                    }

                    if (NewWord == g_FullBlock) {
                        m_SuperBlock[sbIdx].fetch_or(1ULL << i, std::memory_order_relaxed);
                    }

                    size_t ExpectedHint = m_SearchHint.load(std::memory_order_relaxed);
                    if (WordIdx > ExpectedHint) {
                        m_SearchHint.compare_exchange_weak(ExpectedHint, WordIdx,
                                                           std::memory_order_relaxed,
                                                           std::memory_order_relaxed);
                    }

                    SlabDescriptor* Slab = &m_DescriptorSpan[GlobalIdx];
                    Slab->ResetSlab();

                    LOG_ALLOCATOR("DEBUG", "SlabRegistry: Allocated Slab[" << GlobalIdx << "] Word["
                                                                           << WordIdx << "] Bit["
                                                                           << BitIdx << "]");
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
