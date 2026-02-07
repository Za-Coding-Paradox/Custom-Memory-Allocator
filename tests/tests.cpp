#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <modules/allocator_engine.h>
#include <random>
#include <thread>
#include <vector>

using namespace Allocator;
using namespace std::chrono;

// ============================================================================
// TEST FIXTURES
// ============================================================================

class AllocatorEngineTest : public ::testing::Test
{
protected:
    static constexpr size_t g_TestSlabSize = 64 * 1024;
    // 64MB Arena provides enough room for stress but small enough to hit OOM quickly
    static constexpr size_t g_TestArenaSize = 64 * 1024 * 1024;

    AllocatorEngine* m_Engine = nullptr;

    void SetUp() override
    {
        m_Engine = new AllocatorEngine(g_TestSlabSize, g_TestArenaSize);
        m_Engine->Initialize();
    }

    void TearDown() override
    {
        // Engine destructor triggers global cleanup and handles pool destruction
        delete m_Engine;
        m_Engine = nullptr;
    }
};

class HandleSystemTest : public ::testing::Test
{
protected:
    HandleTable* m_Table = nullptr;

    void SetUp() override { m_Table = new HandleTable(1024); }

    void TearDown() override
    {
        delete m_Table;
        m_Table = nullptr;
    }
};

// ============================================================================
// HELPER STRUCTURES
// ============================================================================

struct SmallObject
{
    char data[8];
};
struct Bucket16
{
    char data[16];
};
struct Bucket32
{
    char data[32];
};
struct Bucket64
{
    char data[64];
};
struct Bucket128
{
    char data[128];
};
struct Bucket256
{
    char data[256];
};

struct ComplexObject
{
    int* resource;
    uint64_t magic;

    ComplexObject(int val) : magic(0xCAFEBABE) { resource = new int(val); }
    ~ComplexObject()
    {
        if (resource) {
            delete resource;
            resource = nullptr;
        }
        magic = 0xDEADDEAD;
    }
};

// ============================================================================
// CATEGORY I: THE GAUNTLET (Stress & Concurrency)
// ============================================================================

TEST_F(AllocatorEngineTest, HighFrequencyThrashing_1MillionAllocs_16Threads)
{
    constexpr size_t g_AllocationsPerThread = 1'000'000;
    constexpr size_t g_ThreadCount = 16;
    constexpr size_t g_AllocationSize = 128;

    std::atomic<size_t> g_TotalAllocations{0};

    auto worker = [&]() {
        for (size_t i = 0; i < g_AllocationsPerThread; ++i) {
            void* ptr = m_Engine->Allocate<FrameLoad>(g_AllocationSize);
            if (ptr)
                g_TotalAllocations.fetch_add(1, std::memory_order_relaxed);

            // Periodically reset to reuse slabs via TLS context
            if (i > 0 && (i % 10000 == 0)) {
                m_Engine->Reset<FrameLoad>();
            }
        }
        // CRITICAL: Return slabs to Registry on thread exit
        LinearStrategyModule<FrameLoad>::ShutdownModule();
    };

    auto start = high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();
    auto end = high_resolution_clock::now();

    auto duration = duration_cast<nanoseconds>(end - start).count();
    double nsPerAlloc = static_cast<double>(duration) / g_TotalAllocations.load();

    std::cout << ">> Total Allocations: " << g_TotalAllocations.load() << "\n"
              << ">> Nanoseconds per Allocation: " << nsPerAlloc << " ns\n";

    EXPECT_GT(g_TotalAllocations.load(), 0);
    EXPECT_LT(nsPerAlloc, 2000);
}

TEST_F(AllocatorEngineTest, ContentionStorm_SimultaneousSlabRequests)
{
    constexpr size_t g_ThreadCount = 32;
    std::barrier sync_point(g_ThreadCount);
    std::atomic<size_t> g_SuccessfulSlabGets{0};

    auto worker = [&]() {
        sync_point.arrive_and_wait(); // Force all threads to hit the Registry at once
        void* ptr = m_Engine->Allocate<FrameLoad>(1024);
        if (ptr)
            g_SuccessfulSlabGets.fetch_add(1, std::memory_order_relaxed);
        LinearStrategyModule<FrameLoad>::ShutdownModule();
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    EXPECT_EQ(g_SuccessfulSlabGets.load(), g_ThreadCount);
}

TEST_F(AllocatorEngineTest, SlabExhaustion_FillArena_VerifyOOM_ThenRecover)
{
    constexpr size_t g_LargeAllocationSize = 32 * 1024;

    // Phase 1: Fill entire 64MB Arena
    size_t count = 0;
    while (true) {
        void* ptr = m_Engine->Allocate<GlobalLoad>(g_LargeAllocationSize);
        if (!ptr)
            break;
        count++;
    }

    // Verify OOM
    EXPECT_EQ(m_Engine->Allocate<GlobalLoad>(1024), nullptr);

    // Phase 2: Recovery via Module Shutdown (returns all slabs to global pool)
    LinearStrategyModule<GlobalLoad>::ShutdownModule();

    void* afterRecovery = m_Engine->Allocate<GlobalLoad>(1024);
    EXPECT_NE(afterRecovery, nullptr);
}

// ============================================================================
// CATEGORY II: EDGE CASES & ALIGNMENT
// ============================================================================

TEST_F(AllocatorEngineTest, ZeroByteAllocation_MustReturnNull)
{
    EXPECT_EQ(m_Engine->Allocate<FrameLoad>(0), nullptr);
}

TEST_F(AllocatorEngineTest, AlignmentNightmare_VariousPowersOfTwo)
{
    size_t alignments[] = {1, 8, 16, 64, 128, 512, 4096};
    for (size_t align : alignments) {
        void* ptr = m_Engine->Allocate<GlobalLoad>(128, align);
        ASSERT_NE(ptr, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % align, 0);
    }
    LinearStrategyModule<GlobalLoad>::ShutdownModule();
}

TEST_F(AllocatorEngineTest, HandleStaleness_AllocateFreeResolve)
{
    Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
    ASSERT_NE(h, g_InvalidHandle);

    EXPECT_NE(m_Engine->ResolveHandle<Bucket64>(h), nullptr);
    m_Engine->FreeHandle<PoolScope<Bucket64>>(h);

    // Should be null due to generation mismatch or null pointer in slot
    EXPECT_EQ(m_Engine->ResolveHandle<Bucket64>(h), nullptr);
}

// ============================================================================
// CATEGORY III: TELEMETRY & STATS
// ============================================================================

TEST_F(AllocatorEngineTest, PeakUsageValidation_Sawtooth)
{
    constexpr size_t g_MaxAllocs = 1000;
    for (int cycle = 0; cycle < 5; ++cycle) {
        std::vector<Handle> handles;
        for (size_t i = 0; i < g_MaxAllocs; ++i)
            handles.push_back(m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>());

        for (Handle h : handles)
            m_Engine->FreeHandle<PoolScope<Bucket128>>(h);
    }

    auto stats = PoolModule<BucketScope<128>>::GetStats();
    EXPECT_GE(stats.Peak, g_MaxAllocs * 128);
    EXPECT_LT(stats.Current, stats.Peak);
}

// ============================================================================
// CATEGORY IV: HANDLE SYSTEM DEEP DIVE
// ============================================================================

TEST_F(HandleSystemTest, ConcurrentHandleAllocation_MassiveContention)
{
    constexpr size_t g_ThreadCount = 16;
    constexpr size_t g_HandlesPerThread = 2000;
    std::atomic<size_t> totalAllocated{0};

    auto worker = [&]() {
        std::vector<Handle> local;
        for (size_t i = 0; i < g_HandlesPerThread; ++i) {
            Handle h = m_Table->Allocate(reinterpret_cast<void*>(0xDEADC0DE));
            if (h != g_InvalidHandle) {
                local.push_back(h);
                totalAllocated.fetch_add(1, std::memory_order_relaxed);
            }
        }
        for (Handle h : local)
            m_Table->Free(h);
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    EXPECT_EQ(m_Table->GetActiveCount(), 0);
}

TEST_F(HandleSystemTest, DoubleFree_Safety)
{
    int dummy = 42;
    Handle h = m_Table->Allocate(&dummy);
    EXPECT_TRUE(m_Table->Free(h));
    EXPECT_FALSE(m_Table->Free(h)); // Second free must return false
}

// ============================================================================
// CATEGORY V: LINEAR STRATEGY & REWIND
// ============================================================================

TEST_F(AllocatorEngineTest, LinearScopedMarker_RecursiveRewind)
{
    void* base = m_Engine->Allocate<LevelLoad>(16);
    {
        LinearScopedMarker<LevelLoad> outer;
        m_Engine->Allocate<LevelLoad>(1024);
        {
            LinearScopedMarker<LevelLoad> inner;
            m_Engine->Allocate<LevelLoad>(2048);
            // inner dtor rewinds back to before the 2048 alloc
        }
        void* afterInner = m_Engine->Allocate<LevelLoad>(16);
        // This should reuse the memory where the 2048 alloc was
        EXPECT_LT(reinterpret_cast<uintptr_t>(afterInner),
                  reinterpret_cast<uintptr_t>(base) + 4096);
    }
    LinearStrategyModule<LevelLoad>::ShutdownModule();
}

TEST_F(AllocatorEngineTest, RecursiveOverflow_ChainIntegrity)
{
    constexpr size_t g_AllocSize = 60 * 1024; // Forces a new slab almost every time
    std::vector<void*> ptrs;
    for (int i = 0; i < 20; ++i) {
        void* p = m_Engine->Allocate<FrameLoad>(g_AllocSize);
        ASSERT_NE(p, nullptr);
        std::memset(p, i, 1024);
        ptrs.push_back(p);
    }

    for (int i = 0; i < 20; ++i) {
        unsigned char* data = static_cast<unsigned char*>(ptrs[i]);
        EXPECT_EQ(data[0], static_cast<unsigned char>(i));
    }
    LinearStrategyModule<FrameLoad>::ShutdownModule();
}

// ============================================================================
// CATEGORY VI: CHAOS & FRAGMENTATION
// ============================================================================

TEST_F(AllocatorEngineTest, MixedBuckets_Interleaved)
{
    std::vector<Handle> h16, h64, h256;
    for (int i = 0; i < 1000; ++i) {
        h16.push_back(m_Engine->AllocateWithHandle<Bucket16, PoolScope<Bucket16>>());
        h64.push_back(m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>());
        h256.push_back(m_Engine->AllocateWithHandle<Bucket256, PoolScope<Bucket256>>());
    }

    for (int i = 0; i < 1000; i += 2) {
        m_Engine->FreeHandle<PoolScope<Bucket16>>(h16[i]);
        m_Engine->FreeHandle<PoolScope<Bucket64>>(h64[i]);
        m_Engine->FreeHandle<PoolScope<Bucket256>>(h256[i]);
    }

    // Attempt re-allocation into swiss cheese holes
    // Attempt re-allocation into swiss cheese holes
    for (int i = 0; i < 500; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket16, PoolScope<Bucket16>>();
        EXPECT_NE(h, g_InvalidHandle);
    }
}

// ============================================================================
// CATEGORY VII: SEMANTICS & UTILITIES
// ============================================================================

TEST_F(AllocatorEngineTest, ComplexTypes_PlacementNew_Destructor)
{
    Handle h = m_Engine->AllocateWithHandle<ComplexObject, PoolScope<Bucket64>>();
    void* mem = m_Engine->ResolveHandle<ComplexObject>(h);

    ComplexObject* obj = new (mem) ComplexObject(1337);
    EXPECT_EQ(*obj->resource, 1337);

    obj->~ComplexObject(); // Manual destruction
    EXPECT_EQ(obj->magic, 0xDEADDEAD);

    m_Engine->FreeHandle<PoolScope<Bucket64>>(h);
}

TEST(UtilityTest, PowerOfTwo_Alignment)
{
    EXPECT_TRUE(Utility::IsPowerOfTwo(1024));
    EXPECT_FALSE(Utility::IsPowerOfTwo(1025));
    EXPECT_EQ(Utility::AlignForward((void*)0x1001, 16), (void*)0x1010);
    EXPECT_EQ(Utility::GetPadding((void*)0x1001, 16), 15);
}

// ============================================================================
// CATEGORY VIII: PERFORMANCE REGRESSION
// ============================================================================

TEST_F(AllocatorEngineTest, Performance_LinearSubMicrosecond)
{
    constexpr size_t iterations = 100'000;
    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        m_Engine->Allocate<FrameLoad>(64);
    }
    auto end = high_resolution_clock::now();

    double nsPerAlloc = duration_cast<nanoseconds>(end - start).count() / (double)iterations;
    std::cout << ">> Linear Speed: " << nsPerAlloc << " ns/alloc\n";

    EXPECT_LT(nsPerAlloc, 500); // Typically <100ns in Release, <500ns in Debug
    LinearStrategyModule<FrameLoad>::ShutdownModule();
}

TEST_F(AllocatorEngineTest, Performance_HandleResolution_Blazing)
{
    Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
    constexpr size_t iterations = 1'000'000;

    auto start = high_resolution_clock::now();
    volatile void* sink;
    for (size_t i = 0; i < iterations; ++i) {
        sink = m_Engine->ResolveHandle<Bucket64>(h);
    }
    auto end = high_resolution_clock::now();

    double nsPerResolve = duration_cast<nanoseconds>(end - start).count() / (double)iterations;
    std::cout << ">> Resolution Speed: " << nsPerResolve << " ns/resolve\n";

    EXPECT_LT(nsPerResolve, 50); // Should be a handful of cycles
}

// ============================================================================
// MAIN ENTRY
// ============================================================================

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "\n"
              << "╔══════════════════════════════════════════════════════════════╗\n"
              << "║       ALLOCATOR TORTURE TEST SUITE - REFACTORED (2026)       ║\n"
              << "║       Objective: Stress Unified TLS & Atomic Handshake       ║\n"
              << "╚══════════════════════════════════════════════════════════════╝\n"
              << "\n";
    return RUN_ALL_TESTS();
}
