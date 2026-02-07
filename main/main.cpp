// ============================================================================
// ALLOCATOR PERFORMANCE BENCHMARK SUITE
// Purpose: Generate professional metrics comparing Custom vs System Allocators
// ============================================================================

#include <chrono>
#include <gtest/gtest.h>
#include <iostream>
#include <modules/allocator_engine.h>
#include <vector>

using namespace Allocator;
using namespace std::chrono;

// ============================================================================
// HELPER STRUCTURES (Missing Types Defined Here)
// ============================================================================
struct Bucket8
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

// ============================================================================
// BENCHMARK FIXTURE
// ============================================================================
class AllocatorBenchmark : public ::testing::Test
{
protected:
    static constexpr size_t g_TestSlabSize = 64 * 1024;
    static constexpr size_t g_TestArenaSize = 64 * 1024 * 1024;
    AllocatorEngine* m_Engine = nullptr;

    void SetUp() override
    {
        m_Engine = new AllocatorEngine(g_TestSlabSize, g_TestArenaSize);
        m_Engine->Initialize();
    }

    void TearDown() override
    {
        delete m_Engine;
        m_Engine = nullptr;
    }
};

// ============================================================================
// 1. SPEED BENCHMARK: Custom vs. Malloc
// ============================================================================
TEST_F(AllocatorBenchmark, SpeedThroughput_System_Vs_Custom)
{
    constexpr size_t g_Iterations = 1'000'000;
    constexpr size_t g_AllocSize = 64;

    std::vector<void*> systemPtrs;
    systemPtrs.reserve(g_Iterations);
    std::vector<void*> linearPtrs;
    linearPtrs.reserve(g_Iterations);
    std::vector<Handle> poolHandles;
    poolHandles.reserve(g_Iterations);

    std::cout << "\n=== SPEED BENCHMARK (1 Million Ops) ===\n";

    // --- 1. System Malloc ---
    auto sysStart = high_resolution_clock::now();
    for (size_t i = 0; i < g_Iterations; ++i) {
        systemPtrs.push_back(std::malloc(g_AllocSize));
    }
    auto sysEnd = high_resolution_clock::now();

    auto sysFreeStart = high_resolution_clock::now();
    for (void* ptr : systemPtrs)
        std::free(ptr);
    auto sysFreeEnd = high_resolution_clock::now();

    // --- 2. Custom Linear ---
    m_Engine->Reset<FrameLoad>();
    auto linStart = high_resolution_clock::now();
    for (size_t i = 0; i < g_Iterations; ++i) {
        linearPtrs.push_back(m_Engine->Allocate<FrameLoad>(g_AllocSize));
    }
    auto linEnd = high_resolution_clock::now();

    auto linFreeStart = high_resolution_clock::now();
    m_Engine->Reset<FrameLoad>();
    auto linFreeEnd = high_resolution_clock::now();

    // --- 3. Custom Pool ---
    auto poolStart = high_resolution_clock::now();
    for (size_t i = 0; i < g_Iterations; ++i) {
        poolHandles.push_back(m_Engine->AllocateWithHandle<Bucket64>());
    }
    auto poolEnd = high_resolution_clock::now();

    auto poolFreeStart = high_resolution_clock::now();
    for (Handle h : poolHandles)
        m_Engine->FreeHandle<Bucket64>(h);
    auto poolFreeEnd = high_resolution_clock::now();

    // --- Calculations ---
    auto sysNs =
        duration_cast<nanoseconds>((sysEnd - sysStart) + (sysFreeEnd - sysFreeStart)).count() /
        (double)g_Iterations;
    auto linNs =
        duration_cast<nanoseconds>((linEnd - linStart) + (linFreeEnd - linFreeStart)).count() /
        (double)g_Iterations;
    auto poolNs =
        duration_cast<nanoseconds>((poolEnd - poolStart) + (poolFreeEnd - poolFreeStart)).count() /
        (double)g_Iterations;

    std::cout << "Malloc (Alloc+Free): " << sysNs << " ns/op\n"
              << "Linear (Alloc+Reset): " << linNs << " ns/op (Speedup: " << sysNs / linNs << "x)\n"
              << "Pool   (Alloc+Free) : " << poolNs << " ns/op (Speedup: " << sysNs / poolNs
              << "x)\n";
}

// ============================================================================
// 2. FRAGMENTATION RESISTANCE TEST
// ============================================================================
TEST_F(AllocatorBenchmark, Fragmentation_ZeroGrowth_Verification)
{
    constexpr size_t g_Count = 10'000;
    std::vector<Handle> handles;

    // Phase 1: Fill
    for (size_t i = 0; i < g_Count; ++i)
        handles.push_back(m_Engine->AllocateWithHandle<Bucket64>());

    auto stats1 = PoolModule<BucketScope<64>>::GetStats();
    size_t peakBytes = stats1.Peak;

    // Phase 2: Swiss Cheese (Free every 2nd item)
    size_t freedCount = 0;
    for (size_t i = 0; i < g_Count; i += 2) {
        m_Engine->FreeHandle<Bucket64>(handles[i]);
        freedCount++;
    }

    // Phase 3: Refill (Should use holes)
    for (size_t i = 0; i < freedCount; ++i) {
        handles.push_back(m_Engine->AllocateWithHandle<Bucket64>());
    }

    auto stats2 = PoolModule<BucketScope<64>>::GetStats();

    std::cout << "\n=== FRAGMENTATION TEST ===\n"
              << "Initial Peak : " << peakBytes << " bytes\n"
              << "Final Peak   : " << stats2.Peak << " bytes\n";

    EXPECT_EQ(stats2.Peak, peakBytes) << "Allocator suffered from fragmentation!";
}

// ============================================================================
// 3. OPTIMIZATION VERIFICATION (Hot Slab Cursor)
// ============================================================================
TEST_F(AllocatorBenchmark, Optimization_HotSlabCursor_Jump)
{
    constexpr int ItemsPerSlab = 1024;
    constexpr int SlabCount = 50;
    std::vector<Handle> handles;

    for (int i = 0; i < SlabCount * ItemsPerSlab; ++i) {
        handles.push_back(m_Engine->AllocateWithHandle<Bucket64>());
    }

    Handle target = handles.back();
    m_Engine->FreeHandle<Bucket64>(target);

    auto start = high_resolution_clock::now();
    Handle newH = m_Engine->AllocateWithHandle<Bucket64>();
    auto end = high_resolution_clock::now();

    auto duration = duration_cast<nanoseconds>(end - start).count();
    std::cout << "\n=== CURSOR OPTIMIZATION ===\n"
              << "Time to find slot in 50th slab: " << duration << " ns\n";

    EXPECT_LT(duration, 500);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "\n--- STARTING PERFORMANCE BENCHMARKS ---\n";
    return RUN_ALL_TESTS();
}
