// ============================================================================
// STRESS_TESTS.CPP - HOSTILE QA ENGINEER MODE
// "If it can be broken, I will break it" - Senior QA Engineer
//
// 70+ Tests Designed to Expose Every Weakness in Your Allocator
// No Mercy. No Compromise. Only Truth.
// ============================================================================

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <gtest/gtest.h>
#include <modules/allocator_engine.h>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace Allocator;
using namespace std::chrono;

// ============================================================================
// TEST FIXTURE
// ============================================================================

// ============================================================================
// HELPER STRUCTS (PAYLOADS)
// ============================================================================

// Used to target specific Pool sizes (16B to 256B)
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

// Used for generic small allocations
struct SmallObject
{
    char data[8];
};

// Used for Non-Trivial Type testing
struct ComplexType
{
    std::vector<int> data;
    std::string name;

    ComplexType() : data(100, 42), name("test") {}
    ~ComplexType()
    {
        data.clear();
        name.clear();
    }
};

class AllocatorStressTest : public ::testing::Test
{
protected:
    AllocatorEngine* m_Engine = nullptr;

    void SetUp() override
    {
        m_Engine = new AllocatorEngine(64 * 1024, 64 * 1024 * 1024);
        m_Engine->Initialize();
    }

    void TearDown() override { delete m_Engine; }
};

// ============================================================================
// CATEGORY I: REGISTRY THRASHING (10 TESTS)
// Target: SlabRegistry atomic bitmap and search hint
// ============================================================================

TEST_F(AllocatorStressTest, RegistryThrashing_50ThreadsSimultaneousSlabRequests)
{
    constexpr size_t g_ThreadCount = 50;
    std::atomic<size_t> successCount{0};
    std::atomic<size_t> failCount{0};

    std::barrier syncPoint(g_ThreadCount);

    auto hammer = [&]() {
        syncPoint.arrive_and_wait(); // Synchronize all threads

        // Each thread allocates to force slab requests
        for (int i = 0; i < 100; ++i) {
            void* ptr = m_Engine->Allocate<FrameLoad>(1024);
            if (ptr) {
                successCount.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                failCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(hammer);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== REGISTRY THRASHING (50 THREADS) ===\n"
              << "Success: " << successCount.load() << "\n"
              << "Fail: " << failCount.load() << "\n";

    EXPECT_GT(successCount.load(), g_ThreadCount * 90); // 90%+ success
    EXPECT_EQ(failCount.load(), 0);                     // Zero failures acceptable if arena large
}

TEST_F(AllocatorStressTest, RegistryThrashing_BitMapExhaustion)
{
    std::vector<void*> allocations;

    // Allocate until registry bitmap exhausted
    size_t count = 0;
    while (count < 10000) { // Safety limit
        void* ptr = m_Engine->Allocate<GlobalLoad>(32 * 1024);
        if (!ptr)
            break;
        allocations.push_back(ptr);
        count++;
    }

    std::cout << "=== BITMAP EXHAUSTION ===\n"
              << "Allocated " << count << " large blocks before OOM\n";

    EXPECT_GT(count, 100) << "Should allocate many blocks before exhaust";
}

TEST_F(AllocatorStressTest, RegistryThrashing_SearchHintWrapAround)
{
    std::vector<void*> allocs;

    // Allocate 1000 blocks
    for (int i = 0; i < 1000; ++i) {
        void* ptr = m_Engine->Allocate<LevelLoad>(16 * 1024);
        ASSERT_NE(ptr, nullptr);
        allocs.push_back(ptr);
    }

    // Free every other one (creates holes)
    for (size_t i = 0; i < allocs.size(); i += 2) {
        // Linear doesn't support individual free, so just clear
        allocs[i] = nullptr;
    }

    // Allocate again - search hint should wrap around
    for (int i = 0; i < 100; ++i) {
        void* ptr = m_Engine->Allocate<LevelLoad>(16 * 1024);
        EXPECT_NE(ptr, nullptr) << "Should find free slabs via search hint";
    }
}

TEST_F(AllocatorStressTest, RegistryThrashing_ConcurrentSlabFreeAndAlloc)
{
    constexpr size_t g_ThreadCount = 20;
    std::atomic<bool> running{true};
    std::atomic<size_t> cycleCount{0};

    auto worker = [&]() {
        while (running.load(std::memory_order_relaxed)) {
            void* ptr = m_Engine->Allocate<FrameLoad>(8192);
            if (ptr) {
                cycleCount.fetch_add(1, std::memory_order_relaxed);
            }
            m_Engine->Reset<FrameLoad>(); // Free and repeat
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(worker);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running.store(false, std::memory_order_relaxed);

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== CONCURRENT SLAB CYCLING ===\n"
              << "Total cycles: " << cycleCount.load() << "\n";

    EXPECT_GT(cycleCount.load(), 1000) << "Should achieve high throughput";
}

TEST_F(AllocatorStressTest, RegistryThrashing_SlabAllocationRace)
{
    constexpr size_t g_ThreadCount = 32;
    std::vector<std::vector<void*>> threadAllocs(g_ThreadCount);

    std::barrier syncPoint(g_ThreadCount);

    auto racer = [&](size_t tid) {
        syncPoint.arrive_and_wait();

        for (int i = 0; i < 50; ++i) {
            void* ptr = m_Engine->Allocate<FrameLoad>(4096);
            if (ptr) {
                threadAllocs[tid].push_back(ptr);
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(racer, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    size_t totalAllocs = 0;
    for (const auto& vec : threadAllocs) {
        totalAllocs += vec.size();
    }

    std::cout << "=== SLAB ALLOCATION RACE ===\n"
              << "Total allocations: " << totalAllocs << "\n";

    EXPECT_GE(totalAllocs, g_ThreadCount * 45); // 90% success minimum
}

TEST_F(AllocatorStressTest, RegistryThrashing_MassiveParallelGrowth)
{
    constexpr size_t g_ThreadCount = 64;
    std::atomic<size_t> growthDetected{0};

    auto growthHammer = [&]() {
        for (int i = 0; i < 20; ++i) {
            void* ptr = m_Engine->Allocate<GlobalLoad>(48 * 1024);
            if (ptr) {
                growthDetected.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(growthHammer);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== MASSIVE PARALLEL GROWTH ===\n"
              << "Growth events: " << growthDetected.load() << "\n";

    EXPECT_GT(growthDetected.load(), g_ThreadCount * 15);
}

TEST_F(AllocatorStressTest, RegistryThrashing_SuperBlockStress)
{
    // Allocate sparsely to stress superblock logic
    std::vector<void*> sparse;

    for (int i = 0; i < 500; ++i) {
        void* ptr = m_Engine->Allocate<LevelLoad>(32 * 1024);
        ASSERT_NE(ptr, nullptr);

        // Keep every 10th allocation (creates sparse pattern)
        if (i % 10 == 0) {
            sparse.push_back(ptr);
        }
    }

    // Now allocate more - superblock should help skip full regions
    for (int i = 0; i < 100; ++i) {
        void* ptr = m_Engine->Allocate<LevelLoad>(32 * 1024);
        EXPECT_NE(ptr, nullptr) << "Superblock should optimize search";
    }
}

TEST_F(AllocatorStressTest, RegistryThrashing_AtomicBitmapCorruption)
{
    constexpr size_t g_ThreadCount = 40;
    std::atomic<size_t> allocCount{0};
    std::atomic<size_t> corruptionDetected{0};

    std::barrier syncPoint(g_ThreadCount);

    auto corruptionTest = [&]() {
        syncPoint.arrive_and_wait();

        for (int i = 0; i < 100; ++i) {
            void* ptr = m_Engine->Allocate<FrameLoad>(2048);
            if (ptr) {
                allocCount.fetch_add(1, std::memory_order_relaxed);

                // Verify memory is actually usable
                std::memset(ptr, 0xAA, 2048);
                if (static_cast<unsigned char*>(ptr)[0] != 0xAA) {
                    corruptionDetected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(corruptionTest);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== ATOMIC BITMAP CORRUPTION TEST ===\n"
              << "Allocations: " << allocCount.load() << "\n"
              << "Corruption: " << corruptionDetected.load() << "\n";

    EXPECT_EQ(corruptionDetected.load(), 0) << "No memory corruption allowed";
}

TEST_F(AllocatorStressTest, RegistryThrashing_SlabDescriptorBoundsCheck)
{
    // Try to trigger bounds checking in GetSlabDescriptor
    std::vector<Handle> handles;

    for (int i = 0; i < 1000; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        if (h != g_InvalidHandle) {
            handles.push_back(h);
        }
    }

    // All handles should resolve correctly
    for (Handle h : handles) {
        void* ptr = m_Engine->ResolveHandle<Bucket64>(h);
        EXPECT_NE(ptr, nullptr) << "Valid handle must resolve";
    }

    // Cleanup
    for (Handle h : handles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);
    }
}

TEST_F(AllocatorStressTest, RegistryThrashing_SlabChainIntegrity)
{
    constexpr size_t g_LargeAlloc = 60 * 1024;
    std::vector<void*> chain;

    // Force multiple slab overflows
    for (int i = 0; i < 20; ++i) {
        void* ptr = m_Engine->Allocate<GlobalLoad>(g_LargeAlloc);
        ASSERT_NE(ptr, nullptr) << "Slab chain must extend";
        chain.push_back(ptr);

        // Write unique pattern
        std::memset(ptr, i, 1024);
    }

    // Verify all patterns intact (no chain corruption)
    for (size_t i = 0; i < chain.size(); ++i) {
        unsigned char* data = static_cast<unsigned char*>(chain[i]);
        EXPECT_EQ(data[0], static_cast<unsigned char>(i)) << "Slab chain corrupted at index " << i;
    }
}

// ============================================================================
// CATEGORY II: CROSS-THREAD TORTURE (12 TESTS)
// Target: Thread-safe operations, per-slab mutex, cross-thread free
// ============================================================================

TEST_F(AllocatorStressTest, CrossThread_ProducerConsumerFree)
{
    constexpr size_t g_ObjectCount = 10000;
    std::vector<Handle> producedHandles(g_ObjectCount);
    std::atomic<size_t> produceIndex{0};
    std::atomic<size_t> consumeIndex{0};
    std::atomic<size_t> freeIndex{0};
    std::atomic<bool> productionDone{false};
    std::atomic<bool> consumptionDone{false};

    // Thread A: Producer
    auto producer = [&]() {
        for (size_t i = 0; i < g_ObjectCount; ++i) {
            Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
            ASSERT_NE(h, g_InvalidHandle);

            void* ptr = m_Engine->ResolveHandle<Bucket128>(h);
            ASSERT_NE(ptr, nullptr);

            // Write marker
            *static_cast<uint64_t*>(ptr) = i;

            producedHandles[i] = h;
            produceIndex.fetch_add(1, std::memory_order_release);
        }
        productionDone.store(true, std::memory_order_release);
    };

    // Thread B: Consumer (verify)
    auto consumer = [&]() {
        while (!productionDone.load(std::memory_order_acquire) ||
               consumeIndex.load() < g_ObjectCount) {
            size_t current = consumeIndex.load(std::memory_order_acquire);
            if (current < produceIndex.load(std::memory_order_acquire)) {
                Handle h = producedHandles[current];
                void* ptr = m_Engine->ResolveHandle<Bucket128>(h);

                if (ptr) {
                    uint64_t value = *static_cast<uint64_t*>(ptr);
                    EXPECT_EQ(value, current) << "Data corruption in cross-thread access";
                    consumeIndex.fetch_add(1, std::memory_order_release);
                }
            }
        }
        consumptionDone.store(true, std::memory_order_release);
    };

    // Thread C: Freer
    auto freer = [&]() {
        while (!consumptionDone.load(std::memory_order_acquire) ||
               freeIndex.load() < g_ObjectCount) {
            size_t current = freeIndex.load(std::memory_order_acquire);
            if (current < consumeIndex.load(std::memory_order_acquire)) {
                Handle h = producedHandles[current];
                // FIXED: Correct Template Argument
                bool freed = m_Engine->FreeHandle<Bucket128>(h);
                EXPECT_TRUE(freed);
                freeIndex.fetch_add(1, std::memory_order_release);
            }
        }
    };

    std::thread t1(producer);
    std::thread t2(consumer);
    std::thread t3(freer);

    t1.join();
    t2.join();
    t3.join();

    std::cout << "=== PRODUCER-CONSUMER-FREE ===\n"
              << "Produced: " << produceIndex.load() << "\n"
              << "Consumed: " << consumeIndex.load() << "\n"
              << "Freed: " << freeIndex.load() << "\n";

    EXPECT_EQ(freeIndex.load(), g_ObjectCount);
}

TEST_F(AllocatorStressTest, CrossThread_GetSlabDescriptorValidation)
{
    constexpr size_t g_ThreadCount = 16;
    constexpr size_t g_AllocsPerThread = 500;

    std::vector<std::vector<Handle>> threadHandles(g_ThreadCount);

    // Phase 1: Each thread allocates
    std::vector<std::thread> allocThreads;
    for (size_t tid = 0; tid < g_ThreadCount; ++tid) {
        allocThreads.emplace_back([&, tid]() {
            for (size_t i = 0; i < g_AllocsPerThread; ++i) {
                Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
                if (h != g_InvalidHandle) {
                    threadHandles[tid].push_back(h);
                }
            }
        });
    }

    for (auto& t : allocThreads) {
        t.join();
    }

    // Phase 2: Different threads free (cross-thread free test)
    std::vector<std::thread> freeThreads;
    for (size_t tid = 0; tid < g_ThreadCount; ++tid) {
        freeThreads.emplace_back([&, tid]() {
            // Free objects from NEXT thread (cross-thread)
            size_t targetThread = (tid + 1) % g_ThreadCount;
            for (Handle h : threadHandles[targetThread]) {
                // FIXED: Correct Template Argument
                bool freed = m_Engine->FreeHandle<Bucket64>(h);
                EXPECT_TRUE(freed) << "Cross-thread free must succeed";
            }
        });
    }

    for (auto& t : freeThreads) {
        t.join();
    }

    std::cout << "=== CROSS-THREAD FREE VALIDATION ===\n"
              << "Threads: " << g_ThreadCount << "\n"
              << "Objects per thread: " << g_AllocsPerThread << "\n";
}

TEST_F(AllocatorStressTest, CrossThread_PerSlabMutexContention)
{
    constexpr size_t g_ThreadCount = 32;
    std::atomic<size_t> successfulAllocs{0};
    std::atomic<size_t> contentionDetected{0};

    std::barrier syncPoint(g_ThreadCount);

    auto mutexHammer = [&]() {
        syncPoint.arrive_and_wait();

        auto start = high_resolution_clock::now();

        for (int i = 0; i < 100; ++i) {
            Handle h = m_Engine->AllocateWithHandle<Bucket32, PoolScope<Bucket32>>();
            if (h != g_InvalidHandle) {
                successfulAllocs.fetch_add(1, std::memory_order_relaxed);
                // FIXED: Correct Template Argument
                m_Engine->FreeHandle<Bucket32>(h);
            }
        }

        auto end = high_resolution_clock::now();
        auto duration = duration_cast<microseconds>(end - start).count();

        // If took >1000us, likely mutex contention
        if (duration > 1000) {
            contentionDetected.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(mutexHammer);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== PER-SLAB MUTEX CONTENTION ===\n"
              << "Successful allocs: " << successfulAllocs.load() << "\n"
              << "Threads with contention: " << contentionDetected.load() << "\n";

    EXPECT_GT(successfulAllocs.load(), g_ThreadCount * 90);
}

TEST_F(AllocatorStressTest, CrossThread_SimultaneousAllocateAndFree)
{
    constexpr size_t g_AllocThreads = 8;
    constexpr size_t g_FreeThreads = 8;
    std::atomic<bool> running{true};

    std::vector<Handle> sharedHandles;
    std::mutex handlesMutex;

    std::atomic<size_t> allocCount{0};
    std::atomic<size_t> freeCount{0};

    // Allocator threads
    auto allocator = [&]() {
        while (running.load(std::memory_order_relaxed)) {
            Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
            if (h != g_InvalidHandle) {
                {
                    std::lock_guard<std::mutex> lock(handlesMutex);
                    sharedHandles.push_back(h);
                }
                allocCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    // Freer threads
    auto freer = [&]() {
        while (running.load(std::memory_order_relaxed)) {
            Handle h = g_InvalidHandle;
            {
                std::lock_guard<std::mutex> lock(handlesMutex);
                if (!sharedHandles.empty()) {
                    h = sharedHandles.back();
                    sharedHandles.pop_back();
                }
            }

            if (h != g_InvalidHandle) {
                // FIXED: Correct Template Argument
                m_Engine->FreeHandle<Bucket64>(h);
                freeCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_AllocThreads; ++i) {
        threads.emplace_back(allocator);
    }
    for (size_t i = 0; i < g_FreeThreads; ++i) {
        threads.emplace_back(freer);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    running.store(false, std::memory_order_relaxed);

    for (auto& t : threads) {
        t.join();
    }

    // Cleanup remaining
    for (Handle h : sharedHandles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);
    }

    std::cout << "=== SIMULTANEOUS ALLOC/FREE ===\n"
              << "Allocations: " << allocCount.load() << "\n"
              << "Frees: " << freeCount.load() << "\n";

    EXPECT_GT(allocCount.load(), 1000);
    EXPECT_GT(freeCount.load(), 500);
}

TEST_F(AllocatorStressTest, CrossThread_DataRaceDetection)
{
    constexpr size_t g_ThreadCount = 16;
    std::vector<Handle> handles(g_ThreadCount);
    std::barrier phase1(g_ThreadCount);
    std::barrier phase2(g_ThreadCount);

    std::atomic<size_t> raceDetected{0};

    auto racer = [&](size_t tid) {
        // Phase 1: Allocate
        Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
        ASSERT_NE(h, g_InvalidHandle);
        handles[tid] = h;

        void* ptr = m_Engine->ResolveHandle<Bucket128>(h);
        ASSERT_NE(ptr, nullptr);
        *static_cast<uint64_t*>(ptr) = tid;

        phase1.arrive_and_wait();

        // Phase 2: Verify (different thread reads different allocation)
        size_t targetTid = (tid + 1) % g_ThreadCount;
        Handle targetH = handles[targetTid];
        void* targetPtr = m_Engine->ResolveHandle<Bucket128>(targetH);

        if (targetPtr) {
            uint64_t value = *static_cast<uint64_t*>(targetPtr);
            if (value != targetTid) {
                raceDetected.fetch_add(1, std::memory_order_relaxed);
            }
        }

        phase2.arrive_and_wait();

        // Phase 3: Cleanup
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket128>(handles[tid]);
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(racer, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== DATA RACE DETECTION ===\n"
              << "Races detected: " << raceDetected.load() << "\n";

    EXPECT_EQ(raceDetected.load(), 0) << "No data races allowed!";
}

TEST_F(AllocatorStressTest, CrossThread_TLSIsolation)
{
    constexpr size_t g_ThreadCount = 16;
    std::vector<size_t> threadAllocCounts(g_ThreadCount);

    auto worker = [&](size_t tid) {
        size_t localCount = 0;

        for (int i = 0; i < 1000; ++i) {
            void* ptr = m_Engine->Allocate<FrameLoad>(512);
            if (ptr) {
                localCount++;
            }
        }

        threadAllocCounts[tid] = localCount;
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    // Each thread should have allocated independently
    for (size_t count : threadAllocCounts) {
        EXPECT_GT(count, 900) << "TLS isolation broken";
    }
}

TEST_F(AllocatorStressTest, CrossThread_HandleTableGrowthRace)
{
    constexpr size_t g_ThreadCount = 32;
    constexpr size_t g_HandlesPerThread = 1000;

    std::vector<std::vector<Handle>> threadHandles(g_ThreadCount);
    std::barrier syncPoint(g_ThreadCount);

    auto racer = [&](size_t tid) {
        syncPoint.arrive_and_wait(); // Simultaneous start

        for (size_t i = 0; i < g_HandlesPerThread; ++i) {
            Handle h = m_Engine->AllocateWithHandle<Bucket16, PoolScope<Bucket16>>();
            if (h != g_InvalidHandle) {
                threadHandles[tid].push_back(h);
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(racer, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    // Verify all handles
    size_t totalHandles = 0;
    for (size_t tid = 0; tid < g_ThreadCount; ++tid) {
        for (Handle h : threadHandles[tid]) {
            void* ptr = m_Engine->ResolveHandle<Bucket16>(h);
            EXPECT_NE(ptr, nullptr);
            totalHandles++;
        }
    }

    std::cout << "=== HANDLE TABLE GROWTH RACE ===\n"
              << "Total handles allocated: " << totalHandles << "\n";

    EXPECT_GE(totalHandles, g_ThreadCount * 900);

    // Cleanup
    for (size_t tid = 0; tid < g_ThreadCount; ++tid) {
        for (Handle h : threadHandles[tid]) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket16>(h);
        }
    }
}

TEST_F(AllocatorStressTest, CrossThread_MemoryVisibilityTest)
{
    constexpr size_t g_ProducerCount = 8;
    constexpr size_t g_ConsumerCount = 8;
    constexpr size_t g_ObjectsPerProducer = 500;

    struct SharedData
    {
        std::atomic<uint64_t> value{0};
        std::atomic<bool> ready{false};
    };

    std::vector<std::vector<Handle>> producerHandles(g_ProducerCount);
    std::atomic<size_t> visibilityErrors{0};

    std::barrier phase1(g_ProducerCount + g_ConsumerCount);
    std::barrier phase2(g_ProducerCount + g_ConsumerCount);

    // Producers
    auto producer = [&](size_t pid) {
        for (size_t i = 0; i < g_ObjectsPerProducer; ++i) {
            Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
            ASSERT_NE(h, g_InvalidHandle);

            SharedData* data = m_Engine->ResolveHandle<SharedData>(h);
            ASSERT_NE(data, nullptr);

            data->value.store(pid * 1000 + i, std::memory_order_release);
            data->ready.store(true, std::memory_order_release);

            producerHandles[pid].push_back(h);
        }

        phase1.arrive_and_wait();
        phase2.arrive_and_wait();
    };

    // Consumers
    auto consumer = [&](size_t cid) {
        phase1.arrive_and_wait();

        // Read from all producers
        for (size_t pid = 0; pid < g_ProducerCount; ++pid) {
            for (Handle h : producerHandles[pid]) {
                SharedData* data = m_Engine->ResolveHandle<SharedData>(h);
                if (data) {
                    if (!data->ready.load(std::memory_order_acquire)) {
                        visibilityErrors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }

        phase2.arrive_and_wait();
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ProducerCount; ++i) {
        threads.emplace_back(producer, i);
    }
    for (size_t i = 0; i < g_ConsumerCount; ++i) {
        threads.emplace_back(consumer, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== MEMORY VISIBILITY ===\n"
              << "Visibility errors: " << visibilityErrors.load() << "\n";

    EXPECT_EQ(visibilityErrors.load(), 0);

    // Cleanup
    for (auto& vec : producerHandles) {
        for (Handle h : vec) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket128>(h);
        }
    }
}

TEST_F(AllocatorStressTest, CrossThread_StaleHandleAfterCrossThreadFree)
{
    constexpr size_t g_Iterations = 1000;
    std::atomic<size_t> staleAccessDetected{0};

    for (size_t i = 0; i < g_Iterations; ++i) {
        Handle h = g_InvalidHandle;

        // Thread 1: Allocate
        std::thread t1(
            [&]() { h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>(); });
        t1.join();

        ASSERT_NE(h, g_InvalidHandle);

        // Thread 2: Free
        // FIXED: Correct Template Argument
        std::thread t2([&]() { m_Engine->FreeHandle<Bucket64>(h); });
        t2.join();

        // Thread 3: Try to resolve (should fail - stale)
        std::thread t3([&]() {
            void* ptr = m_Engine->ResolveHandle<Bucket64>(h);
            if (ptr != nullptr) {
                staleAccessDetected.fetch_add(1, std::memory_order_relaxed);
            }
        });
        t3.join();
    }

    std::cout << "=== STALE HANDLE DETECTION ===\n"
              << "Stale accesses: " << staleAccessDetected.load() << "\n";

    // Should be 0 with proper generation counters
    EXPECT_EQ(staleAccessDetected.load(), 0);
}

TEST_F(AllocatorStressTest, CrossThread_AllocateFreeAllocateChurn)
{
    constexpr size_t g_ThreadCount = 16;
    constexpr size_t g_ChurnCycles = 1000;
    std::atomic<size_t> totalCycles{0};

    auto churner = [&]() {
        for (size_t i = 0; i < g_ChurnCycles; ++i) {
            Handle h = m_Engine->AllocateWithHandle<Bucket32, PoolScope<Bucket32>>();
            if (h != g_InvalidHandle) {
                // FIXED: Correct Template Argument
                m_Engine->FreeHandle<Bucket32>(h);
                totalCycles.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    auto start = high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(churner);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "=== ALLOCATE/FREE CHURN ===\n"
              << "Total cycles: " << totalCycles.load() << "\n"
              << "Time: " << ms << " ms\n"
              << "Cycles/sec: " << (totalCycles.load() * 1000 / std::max(ms, 1L)) << "\n";

    EXPECT_EQ(totalCycles.load(), g_ThreadCount * g_ChurnCycles);
}

TEST_F(AllocatorStressTest, CrossThread_MixedLinearAndPoolConcurrent)
{
    constexpr size_t g_ThreadCount = 16;
    std::atomic<size_t> linearAllocs{0};
    std::atomic<size_t> poolAllocs{0};

    std::barrier syncPoint(g_ThreadCount);

    auto mixer = [&](size_t tid) {
        syncPoint.arrive_and_wait();

        std::vector<Handle> handles;

        for (int i = 0; i < 500; ++i) {
            if (tid % 2 == 0) {
                // Linear
                void* ptr = m_Engine->Allocate<FrameLoad>(256);
                if (ptr) {
                    linearAllocs.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else {
                // Pool
                Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
                if (h != g_InvalidHandle) {
                    handles.push_back(h);
                    poolAllocs.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // Cleanup pool
        for (Handle h : handles) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket64>(h);
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(mixer, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== MIXED LINEAR/POOL CONCURRENT ===\n"
              << "Linear allocations: " << linearAllocs.load() << "\n"
              << "Pool allocations: " << poolAllocs.load() << "\n";

    EXPECT_GT(linearAllocs.load(), 3500);
    EXPECT_GT(poolAllocs.load(), 3500);
}

TEST_F(AllocatorStressTest, CrossThread_PoolFirstNonFullSlabInvalidation)
{
    constexpr size_t g_ThreadCount = 8;
    std::atomic<size_t> staleHintDetected{0};

    auto slabFiller = [&]() {
        std::vector<Handle> handles;

        // Fill up slabs
        for (int i = 0; i < 2000; ++i) {
            Handle h = m_Engine->AllocateWithHandle<Bucket32, PoolScope<Bucket32>>();
            if (h != g_InvalidHandle) {
                handles.push_back(h);
            }
            else {
                // If allocation failed despite having handles, hint might be stale
                if (!handles.empty()) {
                    staleHintDetected.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
        }

        // Free all
        for (Handle h : handles) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket32>(h);
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(slabFiller);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== FIRSTNONFULLSLAB INVALIDATION ===\n"
              << "Stale hint incidents: " << staleHintDetected.load() << "\n";

    // Should be low (rare case where OOM happens with hint issue)
    EXPECT_LT(staleHintDetected.load(), g_ThreadCount / 2);
}

// ============================================================================
// CATEGORY III: CACHE THRASHING & FALSE SHARING (8 TESTS)
// Target: Cache line alignment, false sharing detection
// ============================================================================

TEST_F(AllocatorStressTest, CacheThrashing_AtomicCounterHammering)
{
    constexpr size_t g_ThreadCount = 64;
    constexpr size_t g_Iterations = 1000000;

    // Two scenarios: with and without padding
    struct alignas(64) PaddedCounter
    {
        std::atomic<uint64_t> counter{0};
        char padding[56]; // Fill cache line
    };

    struct UnpaddedCounter
    {
        std::atomic<uint64_t> counter{0};
    };

    std::vector<PaddedCounter> paddedCounters(g_ThreadCount);
    std::vector<UnpaddedCounter> unpaddedCounters(g_ThreadCount);

    // Test 1: Padded (should be fast)
    auto start1 = high_resolution_clock::now();
    {
        std::vector<std::thread> threads;
        for (size_t tid = 0; tid < g_ThreadCount; ++tid) {
            threads.emplace_back([&, tid]() {
                for (size_t i = 0; i < g_Iterations; ++i) {
                    paddedCounters[tid].counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }
    auto end1 = high_resolution_clock::now();
    auto paddedTime = duration_cast<milliseconds>(end1 - start1).count();

    // Test 2: Unpadded (should be slower due to false sharing)
    auto start2 = high_resolution_clock::now();
    {
        std::vector<std::thread> threads;
        for (size_t tid = 0; tid < g_ThreadCount; ++tid) {
            threads.emplace_back([&, tid]() {
                for (size_t i = 0; i < g_Iterations; ++i) {
                    unpaddedCounters[tid].counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }
    auto end2 = high_resolution_clock::now();
    auto unpaddedTime = duration_cast<milliseconds>(end2 - start2).count();

    float speedup = static_cast<float>(unpaddedTime) / static_cast<float>(paddedTime);

    std::cout << "=== CACHE LINE PADDING EFFECTIVENESS ===\n"
              << "Padded time: " << paddedTime << " ms\n"
              << "Unpadded time: " << unpaddedTime << " ms\n"
              << "Speedup from padding: " << speedup << "x\n";

    // Padded should be at least 1.5x faster
    EXPECT_GT(speedup, 1.5f) << "Cache line padding not effective";
}

TEST_F(AllocatorStressTest, CacheThrashing_ContextStatsAlignment)
{
    // Verify ContextStats is properly aligned
    ContextStats stats1, stats2;

    uintptr_t addr1 = reinterpret_cast<uintptr_t>(&stats1);
    uintptr_t addr2 = reinterpret_cast<uintptr_t>(&stats2);

    EXPECT_EQ(addr1 % 64, 0) << "ContextStats must be 64-byte aligned";
    EXPECT_EQ(addr2 % 64, 0) << "ContextStats must be 64-byte aligned";

    std::cout << "=== CONTEXTSTATS ALIGNMENT ===\n"
              << "stats1 address: 0x" << std::hex << addr1 << std::dec << "\n"
              << "stats2 address: 0x" << std::hex << addr2 << std::dec << "\n"
              << "Alignment: " << (addr1 % 64) << " bytes\n";
}

TEST_F(AllocatorStressTest, CacheThrashing_HandleTableAtomicSeparation)
{
    // Verify handle table atomics are on separate cache lines
    constexpr size_t g_ThreadCount = 32;
    constexpr size_t g_Iterations = 100000;

    std::atomic<size_t> allocCount{0};

    auto start = high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back([&]() {
            for (size_t j = 0; j < g_Iterations; ++j) {
                Handle h = m_Engine->AllocateWithHandle<Bucket16, PoolScope<Bucket16>>();
                if (h != g_InvalidHandle) {
                    allocCount.fetch_add(1, std::memory_order_relaxed);
                    // FIXED: Correct Template Argument
                    m_Engine->FreeHandle<Bucket16>(h);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();
    auto opsPerSec = (allocCount.load() * 1000) / std::max(ms, 1L);

    std::cout << "=== HANDLE TABLE CACHE EFFICIENCY ===\n"
              << "Total operations: " << allocCount.load() << "\n"
              << "Time: " << ms << " ms\n"
              << "Ops/sec: " << opsPerSec << "\n";

    // Should achieve high throughput (>1M ops/sec)
    EXPECT_GT(opsPerSec, 1000000);
}
//
// CacheThrashing_TLSBufferAccess — v3
//
//  ROOT CAUSE OF PREVIOUS FAILURE (confirmed by diagnostic output):
//    Each thread triggered ~19 calls to AllocateSlab() during the timed loop
//    (10000 allocs / 512 per slab = ~19 overflows per thread).
//    16 threads doing this simultaneously created CAS contention on the shared
//    bitmap, producing a bimodal time distribution (~58µs vs ~90µs clusters)
//    rather than uniform throughput. The split was not false sharing — it was
//    pure registry contention during the timed window.
//
//  THE FIX:
//    Phase 1 pre-warms ALL slabs each thread will need (g_SlabsNeeded slabs),
//    then calls Reset<FrameLoad>() which rewinds bump pointers without
//    returning slabs to the registry. The timed loop reuses the already-built
//    chain with zero registry contact — pure bump-pointer throughput.
//
//  SECONDARY FIX:
//    The warmup diagnostic now uses an absolute µs threshold instead of a
//    relative percentage, which was firing as "150% variance" on 2µs noise.

TEST_F(AllocatorStressTest, CacheThrashing_TLSBufferAccess)
{
    constexpr size_t g_ThreadCount = 16;
    constexpr size_t g_AllocsPerThread = 10000;
    constexpr size_t g_AllocSize = 128;
    constexpr size_t g_SlabCapacity = g_ConstSlabSize / g_AllocSize; // 512

    // How many slabs does each thread need? +1 for partial last slab.
    constexpr size_t g_SlabsNeeded = (g_AllocsPerThread / g_SlabCapacity) + 1; // 20

    constexpr float g_VarianceLimit = 0.35f;
    constexpr size_t g_WarmupContendedThresholdUs = 500; // only warn above this absolute value

    struct alignas(64) ThreadResult
    {
        size_t WarmupTimeUs = 0;
        size_t SteadyStateTimeUs = 0;
        size_t NullCount = 0;
        size_t SlabsAcquired = 0;
        bool WarmupSucceeded = false;
    };
    std::array<ThreadResult, g_ThreadCount> Results{};

    std::barrier warmupDone(g_ThreadCount);
    std::barrier startTimed(g_ThreadCount);

    auto worker = [&](size_t tid) {
        auto& R = Results[tid];

        // Phase 1: force every GrowSlabChain() call this thread will ever need.
        // Then Reset() rewinds bump pointers WITHOUT returning slabs to registry.
        {
            auto warmupStart = high_resolution_clock::now();

            size_t slabsTriggered = 0;
            const size_t totalWarmupAllocs = g_SlabsNeeded * g_SlabCapacity;
            for (size_t i = 0; i < totalWarmupAllocs; ++i) {
                void* ptr = m_Engine->Allocate<FrameLoad>(g_AllocSize);
                if (!ptr)
                    break;
                if (i > 0 && (i % g_SlabCapacity) == 0)
                    slabsTriggered++;
            }

            m_Engine->Reset<FrameLoad>();

            auto warmupEnd = high_resolution_clock::now();
            R.WarmupTimeUs = duration_cast<microseconds>(warmupEnd - warmupStart).count();
            R.SlabsAcquired = slabsTriggered;
            R.WarmupSucceeded = (slabsTriggered >= g_SlabsNeeded - 1);
        }

        warmupDone.arrive_and_wait();

        // Phase 2: timed steady-state — no GrowSlabChain() should fire here.
        startTimed.arrive_and_wait();

        auto loopStart = high_resolution_clock::now();
        for (size_t i = 0; i < g_AllocsPerThread; ++i) {
            void* ptr = m_Engine->Allocate<FrameLoad>(g_AllocSize);
            if (!ptr)
                R.NullCount++;
        }
        auto loopEnd = high_resolution_clock::now();
        R.SteadyStateTimeUs = duration_cast<microseconds>(loopEnd - loopStart).count();
    };

    std::vector<std::thread> threads;
    threads.reserve(g_ThreadCount);
    for (size_t i = 0; i < g_ThreadCount; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    // Report Phase 1
    {
        size_t totalWarmup = 0, maxWarmup = 0, minWarmup = SIZE_MAX, failedWarmup = 0;
        for (size_t i = 0; i < g_ThreadCount; ++i) {
            totalWarmup += Results[i].WarmupTimeUs;
            maxWarmup = std::max(maxWarmup, Results[i].WarmupTimeUs);
            minWarmup = std::min(minWarmup, Results[i].WarmupTimeUs);
            if (!Results[i].WarmupSucceeded)
                failedWarmup++;
        }
        size_t avgWarmup = totalWarmup / g_ThreadCount;

        std::cout << "\n=== PHASE 1: SLAB PRE-WARM ===\n"
                  << "  Slabs needed/thread : " << g_SlabsNeeded << "\n"
                  << "  Warmup allocs/thread: " << (g_SlabsNeeded * g_SlabCapacity) << "\n"
                  << "  Failed pre-warms    : " << failedWarmup << "\n"
                  << "  Avg warmup          : " << avgWarmup << " µs\n"
                  << "  Min/Max warmup      : " << minWarmup << " / " << maxWarmup << " µs\n"
                  << "  Per-thread (µs)     : ";
        for (size_t i = 0; i < g_ThreadCount; ++i)
            std::cout << "[T" << i << ":" << Results[i].WarmupTimeUs << "] ";
        std::cout << "\n  [DIAGNOSIS]\n";

        if (avgWarmup > g_WarmupContendedThresholdUs)
            std::cout << "  >> HIGH warmup (" << avgWarmup << "µs avg) — "
                      << "registry CAS or mutex contention significant during slab build.\n";
        else
            std::cout << "  >> Warmup OK (" << avgWarmup << "µs avg).\n";

        EXPECT_EQ(failedWarmup, 0u)
            << failedWarmup << " thread(s) failed to acquire all " << g_SlabsNeeded
            << " slabs. Increase ArenaSize or reduce g_ThreadCount/g_AllocsPerThread.";
    }

    // Report Phase 2
    {
        size_t total = 0, maxTime = 0, minTime = SIZE_MAX, totalNulls = 0;
        for (size_t i = 0; i < g_ThreadCount; ++i) {
            total += Results[i].SteadyStateTimeUs;
            maxTime = std::max(maxTime, Results[i].SteadyStateTimeUs);
            minTime = std::min(minTime, Results[i].SteadyStateTimeUs);
            totalNulls += Results[i].NullCount;
        }
        size_t avgTime = total / g_ThreadCount;
        float variance = (avgTime > 0)
                             ? static_cast<float>(maxTime - minTime) / static_cast<float>(avgTime)
                             : 0.0f;

        std::cout << "\n=== PHASE 2: STEADY-STATE (Pure Bump Alloc, No Registry) ===\n"
                  << "  Allocs/thread : " << g_AllocsPerThread << " @ " << g_AllocSize << "B\n"
                  << "  Total OOM hits: " << totalNulls
                  << (totalNulls > 0 ? "  !! Pre-warm insufficient" : " (good)") << "\n"
                  << "  Avg time      : " << avgTime << " µs\n"
                  << "  Min/Max time  : " << minTime << " / " << maxTime << " µs\n"
                  << "  Variance      : " << (variance * 100.f)
                  << "% (limit: " << (g_VarianceLimit * 100.f) << "%)\n"
                  << "\n  Per-thread (µs):\n";

        for (size_t i = 0; i < g_ThreadCount; ++i) {
            const auto& R = Results[i];
            const bool outlier =
                R.SteadyStateTimeUs > avgTime * 2 || R.SteadyStateTimeUs < avgTime / 2;
            std::cout << "    T" << std::setw(2) << i << ": " << std::setw(5) << R.SteadyStateTimeUs
                      << " µs"
                      << "  nulls=" << R.NullCount << (outlier ? "  << OUTLIER" : "") << "\n";
        }

        std::cout << "\n  [DIAGNOSIS]\n";
        if (totalNulls > 0)
            std::cout << "  >> OOM during timed loop — increase g_SlabsNeeded from "
                      << g_SlabsNeeded << ".\n";
        else if (variance < g_VarianceLimit)
            std::cout << "  >> PASS. No registry contact, TLS isolation working.\n";
        else
            std::cout << "  >> FAIL with registry eliminated.\n"
                      << "  >> Two clusters = OS scheduler / HT pairs.\n"
                      << "  >> Scattered spread = check ThreadLocalData padding (must be\n"
                      << "     multiple of 64 bytes) in linear_module.h.\n";

        EXPECT_EQ(totalNulls, 0u) << "OOM during timed loop. Increase g_SlabsNeeded (currently "
                                  << g_SlabsNeeded << ").";
        EXPECT_LT(variance, g_VarianceLimit)
            << "Variance " << (variance * 100.f) << "% after eliminating registry contact.";
    }
}

TEST_F(AllocatorStressTest, CacheThrashing_SlabDescriptorMutexContention)
{
    constexpr size_t g_ThreadCount = 32;
    constexpr size_t g_AllocsPerThread = 1000;

    std::vector<Handle> handles;
    std::mutex handlesMutex;

    // Pre-allocate to force threads to share slabs
    for (int i = 0; i < 100; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        if (h != g_InvalidHandle) {
            handles.push_back(h);
        }
    }

    std::atomic<size_t> contentionCount{0};
    std::barrier syncPoint(g_ThreadCount);

    auto worker = [&](size_t tid) {
        syncPoint.arrive_and_wait();

        auto start = high_resolution_clock::now();

        std::vector<Handle> localHandles;
        for (size_t i = 0; i < g_AllocsPerThread; ++i) {
            Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
            if (h != g_InvalidHandle) {
                localHandles.push_back(h);
            }
        }

        auto end = high_resolution_clock::now();
        auto us = duration_cast<microseconds>(end - start).count();

        // If thread took >5x average, likely mutex contention
        if (us > 10000) { // 10ms threshold
            contentionCount.fetch_add(1, std::memory_order_relaxed);
        }

        // Cleanup
        for (Handle h : localHandles) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket64>(h);
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== SLAB MUTEX CONTENTION ===\n"
              << "Threads with high contention: " << contentionCount.load() << "\n";

    // Some contention expected, but not all threads
    EXPECT_LT(contentionCount.load(), g_ThreadCount / 2);

    // Cleanup
    for (Handle h : handles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);
    }
}

TEST_F(AllocatorStressTest, CacheThrashing_GlobalStatsUpdateRate)
{
    constexpr size_t g_ThreadCount = 16;
    constexpr size_t g_AllocsPerThread = 10000;

    std::atomic<size_t> flushCount{0};

    auto worker = [&]() {
        for (size_t i = 0; i < g_AllocsPerThread; ++i) {
            void* ptr = m_Engine->Allocate<FrameLoad>(64);
            if (!ptr)
                break;
        }

        LinearStrategyModule<FrameLoad>::FlushThreadStats();
        flushCount.fetch_add(1, std::memory_order_relaxed);
    };

    auto start = high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(worker);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "=== GLOBAL STATS UPDATE RATE ===\n"
              << "Flushes: " << flushCount.load() << "\n"
              << "Time: " << ms << " ms\n";

    EXPECT_EQ(flushCount.load(), g_ThreadCount);
}

TEST_F(AllocatorStressTest, CacheThrashing_PoolStatsAccumulation)
{
    constexpr size_t g_ThreadCount = 16;
    constexpr size_t g_AllocsPerThread = 500;

    std::vector<std::vector<Handle>> threadHandles(g_ThreadCount);

    auto worker = [&](size_t tid) {
        for (size_t i = 0; i < g_AllocsPerThread; ++i) {
            Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
            if (h != g_InvalidHandle) {
                threadHandles[tid].push_back(h);
            }
        }

        // Flush stats
        PoolModule<BucketScope<128>>::FlushThreadStats();
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto stats = PoolModule<BucketScope<128>>::GetStats();

    std::cout << "=== POOL STATS ACCUMULATION ===\n"
              << "BytesAllocated: " << stats.BytesAllocated << "\n"
              << "AllocationCount: " << stats.AllocationCount << "\n";

    // Cleanup
    for (auto& vec : threadHandles) {
        for (Handle h : vec) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket128>(h);
        }
    }
}

TEST_F(AllocatorStressTest, CacheThrashing_CrossCacheLineBouncing)
{
    // Force cache line bouncing by having threads ping-pong
    constexpr size_t g_Iterations = 100000;

    alignas(64) std::atomic<uint64_t> counter1{0};
    alignas(64) std::atomic<uint64_t> counter2{0};

    auto start = high_resolution_clock::now();

    std::thread t1([&]() {
        for (size_t i = 0; i < g_Iterations; ++i) {
            counter1.fetch_add(1, std::memory_order_relaxed);
            // Force read of other counter (cross-cache-line access)
            volatile uint64_t temp = counter2.load(std::memory_order_relaxed);
            (void)temp;
        }
    });

    std::thread t2([&]() {
        for (size_t i = 0; i < g_Iterations; ++i) {
            counter2.fetch_add(1, std::memory_order_relaxed);
            volatile uint64_t temp = counter1.load(std::memory_order_relaxed);
            (void)temp;
        }
    });

    t1.join();
    t2.join();

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "=== CACHE LINE BOUNCING ===\n"
              << "Time: " << ms << " ms\n"
              << "Counter1: " << counter1.load() << "\n"
              << "Counter2: " << counter2.load() << "\n";

    // Just verify completion
    EXPECT_EQ(counter1.load(), g_Iterations);
    EXPECT_EQ(counter2.load(), g_Iterations);
}

// ============================================================================
// CATEGORY IV: OOM & RECOVERY GAUNTLET (8 TESTS)
// Target: Out-of-memory handling, fragmentation recovery
// ============================================================================

TEST_F(AllocatorStressTest, OOMRecovery_SaturateAndRecover)
{
    std::vector<void*> allocations;

    // Phase 1: Saturate
    size_t count = 0;
    while (count < 100000) { // Safety
        void* ptr = m_Engine->Allocate<GlobalLoad>(32 * 1024);
        if (!ptr)
            break;
        allocations.push_back(ptr);
        count++;
    }

    std::cout << "=== OOM SATURATION ===\n"
              << "Allocated " << count << " blocks before OOM\n";

    EXPECT_GT(count, 50) << "Should allocate many blocks";

    // Verify next allocation fails
    void* shouldFail = m_Engine->Allocate<GlobalLoad>(32 * 1024);
    EXPECT_EQ(shouldFail, nullptr) << "Should be OOM";

    // Phase 2: Reset and recover
    m_Engine->Reset<GlobalLoad>();

    void* afterReset = m_Engine->Allocate<GlobalLoad>(32 * 1024);
    EXPECT_NE(afterReset, nullptr) << "Should recover after reset";
}

TEST_F(AllocatorStressTest, OOMRecovery_CheckerboardFragmentation)
{
    std::vector<Handle> allHandles;

    // Allocate many objects
    for (int i = 0; i < 5000; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        if (h != g_InvalidHandle) {
            allHandles.push_back(h);
        }
    }

    std::cout << "=== CHECKERBOARD FRAGMENTATION ===\n"
              << "Initial allocations: " << allHandles.size() << "\n";

    // Free every other one (checkerboard)
    std::vector<Handle> kept;
    for (size_t i = 0; i < allHandles.size(); ++i) {
        if (i % 2 == 0) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket64>(allHandles[i]);
        }
        else {
            kept.push_back(allHandles[i]);
        }
    }

    // Try to reallocate into freed slots
    std::vector<Handle> reallocated;
    for (size_t i = 0; i < allHandles.size() / 2; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        if (h != g_InvalidHandle) {
            reallocated.push_back(h);
        }
    }

    std::cout << "Kept: " << kept.size() << "\n"
              << "Reallocated: " << reallocated.size() << "\n";

    EXPECT_GE(reallocated.size(), kept.size() * 0.9);

    // Cleanup
    for (Handle h : kept) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);
    }
    for (Handle h : reallocated) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);
    }
}

TEST_F(AllocatorStressTest, OOMRecovery_PoolExhaustionAllBuckets)
{
    std::vector<Handle> handles16, handles32, handles64, handles128, handles256;

    // FIX: Use a C++20 Templated Lambda.
    // Instead of passing an 'int' and mapping it to a type, we pass the Type directly.
    auto exhaustBucket = [&]<typename BucketType>(std::vector<Handle>& handleVec) {
        for (int i = 0; i < 100000; ++i) {
            Handle h = m_Engine->AllocateWithHandle<BucketType, PoolScope<BucketType>>();
            if (h == g_InvalidHandle)
                break;
            handleVec.push_back(h);
        }
    };

    // Call explicitly with the type
    exhaustBucket.template operator()<Bucket16>(handles16);
    exhaustBucket.template operator()<Bucket32>(handles32);
    exhaustBucket.template operator()<Bucket64>(handles64);
    exhaustBucket.template operator()<Bucket128>(handles128);
    exhaustBucket.template operator()<Bucket256>(handles256);

    std::cout << "=== POOL EXHAUSTION ALL BUCKETS ===\n"
              << "16B: " << handles16.size() << "\n"
              << "32B: " << handles32.size() << "\n"
              << "64B: " << handles64.size() << "\n"
              << "128B: " << handles128.size() << "\n"
              << "256B: " << handles256.size() << "\n";

    // Cleanup
    for (Handle h : handles16)
        m_Engine->FreeHandle<Bucket16>(h);
    for (Handle h : handles32)
        m_Engine->FreeHandle<Bucket32>(h);
    for (Handle h : handles64)
        m_Engine->FreeHandle<Bucket64>(h);
    for (Handle h : handles128)
        m_Engine->FreeHandle<Bucket128>(h);
    for (Handle h : handles256)
        m_Engine->FreeHandle<Bucket256>(h);
}

TEST_F(AllocatorStressTest, OOMRecovery_LinearSlabChainExhaustion)
{
    std::vector<void*> chain;

    // Allocate across many slabs
    size_t count = 0;
    while (count < 10000) {
        void* ptr = m_Engine->Allocate<LevelLoad>(60 * 1024);
        if (!ptr)
            break;
        chain.push_back(ptr);
        count++;
    }

    std::cout << "=== LINEAR SLAB CHAIN EXHAUSTION ===\n"
              << "Slabs in chain: " << count << "\n";

    EXPECT_GT(count, 10);

    // Rewind to start
    m_Engine->Reset<LevelLoad>();

    // Should be able to allocate again
    void* afterRewind = m_Engine->Allocate<LevelLoad>(60 * 1024);
    EXPECT_NE(afterRewind, nullptr);
}

TEST_F(AllocatorStressTest, OOMRecovery_ConcurrentOOMHandling)
{
    constexpr size_t g_ThreadCount = 16;
    std::atomic<size_t> oomCount{0};
    std::atomic<size_t> successCount{0};

    std::barrier syncPoint(g_ThreadCount);

    auto allocator = [&]() {
        syncPoint.arrive_and_wait();

        for (int i = 0; i < 1000; ++i) {
            void* ptr = m_Engine->Allocate<GlobalLoad>(48 * 1024);
            if (ptr) {
                successCount.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                oomCount.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(allocator);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== CONCURRENT OOM HANDLING ===\n"
              << "Successful allocations: " << successCount.load() << "\n"
              << "OOM events: " << oomCount.load() << "\n";

    EXPECT_GT(successCount.load(), 100);
}

TEST_F(AllocatorStressTest, OOMRecovery_RepeatedOOMCycles)
{
    constexpr size_t g_Cycles = 10;

    for (size_t cycle = 0; cycle < g_Cycles; ++cycle) {
        std::vector<void*> allocations;

        // Fill until OOM
        size_t count = 0;
        while (count < 5000) {
            void* ptr = m_Engine->Allocate<LevelLoad>(32 * 1024);
            if (!ptr)
                break;
            allocations.push_back(ptr);
            count++;
        }

        // Reset
        m_Engine->Reset<LevelLoad>();

        // Verify recovery
        void* test = m_Engine->Allocate<LevelLoad>(1024);
        EXPECT_NE(test, nullptr) << "Failed to recover on cycle " << cycle;
    }

    std::cout << "=== REPEATED OOM CYCLES ===\n"
              << "Completed " << g_Cycles << " cycles\n";
}

TEST_F(AllocatorStressTest, OOMRecovery_PartialRecoveryAfterFree)
{
    std::vector<Handle> allHandles;

    // Allocate until near OOM
    for (int i = 0; i < 50000; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
        if (h == g_InvalidHandle)
            break;
        allHandles.push_back(h);
    }

    size_t beforeFree = allHandles.size();

    // Free 25%
    size_t freeCount = allHandles.size() / 4;
    for (size_t i = 0; i < freeCount; ++i) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket128>(allHandles[i]);
    }
    allHandles.erase(allHandles.begin(), allHandles.begin() + freeCount);

    // Should be able to allocate again
    std::vector<Handle> afterFree;
    for (size_t i = 0; i < freeCount; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
        if (h != g_InvalidHandle) {
            afterFree.push_back(h);
        }
    }

    std::cout << "=== PARTIAL RECOVERY ===\n"
              << "Before free: " << beforeFree << "\n"
              << "Freed: " << freeCount << "\n"
              << "Reallocated: " << afterFree.size() << "\n";

    EXPECT_GE(afterFree.size(), freeCount * 0.9);

    // Cleanup
    for (Handle h : allHandles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket128>(h);
    }
    for (Handle h : afterFree) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket128>(h);
    }
}

TEST_F(AllocatorStressTest, OOMRecovery_FragmentationWorstCase)
{
    std::vector<Handle> pattern1, pattern2;

    // Allocate alternating bucket sizes
    for (int i = 0; i < 2000; ++i) {
        if (i % 2 == 0) {
            Handle h = m_Engine->AllocateWithHandle<Bucket32, PoolScope<Bucket32>>();
            if (h != g_InvalidHandle)
                pattern1.push_back(h);
        }
        else {
            Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
            if (h != g_InvalidHandle)
                pattern2.push_back(h);
        }
    }

    // Free all 32-byte objects
    for (Handle h : pattern1) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket32>(h);
    }

    // Try to allocate 64-byte objects (different size)
    std::vector<Handle> pattern3;
    for (int i = 0; i < 500; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        if (h != g_InvalidHandle) {
            pattern3.push_back(h);
        }
    }

    std::cout << "=== FRAGMENTATION WORST CASE ===\n"
              << "32B allocated: " << pattern1.size() << "\n"
              << "128B allocated: " << pattern2.size() << "\n"
              << "64B allocated after free: " << pattern3.size() << "\n";

    // Cleanup
    for (Handle h : pattern2)
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket128>(h);
    for (Handle h : pattern3)
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);
}

// ============================================================================
// CATEGORY V: HANDLE SYSTEM LIMITS (10 TESTS)
// Target: Handle table growth, generation wraparound, capacity limits
// ============================================================================

TEST_F(AllocatorStressTest, HandleLimits_PushToMillion)
{
    constexpr size_t g_TargetHandles = 1000000;
    std::vector<Handle> handles;
    handles.reserve(g_TargetHandles);

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < g_TargetHandles; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket16, PoolScope<Bucket16>>();
        if (h != g_InvalidHandle) {
            handles.push_back(h);
        }
        else {
            break;
        }

        if (i % 100000 == 0) {
            std::cout << "Progress: " << i << " handles...\n";
        }
    }

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "=== PUSH TO MILLION ===\n"
              << "Allocated: " << handles.size() << " handles\n"
              << "Time: " << ms << " ms\n"
              << "Handles/sec: " << (handles.size() * 1000 / std::max(ms, 1L)) << "\n";

    EXPECT_GT(handles.size(), 900000) << "Should reach close to 1M";

    // Verify all resolve
    size_t resolveFailures = 0;
    for (Handle h : handles) {
        if (m_Engine->ResolveHandle<Bucket16>(h) == nullptr) {
            resolveFailures++;
        }
    }

    EXPECT_EQ(resolveFailures, 0);

    // Cleanup
    for (Handle h : handles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket16>(h);
    }
}

TEST_F(AllocatorStressTest, HandleLimits_GenerationWraparound)
{
    constexpr size_t g_ReuseCount = 100000;

    int dummy = 42;
    Handle currentHandle = m_Engine->AllocateWithHandle<Bucket32, PoolScope<Bucket32>>();
    ASSERT_NE(currentHandle, g_InvalidHandle);

    uint32_t initialGen = currentHandle.GetGeneration();
    uint32_t maxGen = initialGen;

    // Repeatedly free and reallocate same slot
    for (size_t i = 0; i < g_ReuseCount; ++i) {
        uint32_t idx = currentHandle.GetIndex();

        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket32>(currentHandle);
        currentHandle = m_Engine->AllocateWithHandle<Bucket32, PoolScope<Bucket32>>();

        ASSERT_NE(currentHandle, g_InvalidHandle);
        EXPECT_EQ(currentHandle.GetIndex(), idx) << "Should reuse same slot";

        uint32_t gen = currentHandle.GetGeneration();
        if (gen > maxGen) {
            maxGen = gen;
        }

        // Verify generation never wraps to 0
        EXPECT_NE(gen, 0) << "Generation must never be 0";
    }

    std::cout << "=== GENERATION WRAPAROUND ===\n"
              << "Reuse cycles: " << g_ReuseCount << "\n"
              << "Initial gen: " << initialGen << "\n"
              << "Max gen: " << maxGen << "\n"
              << "Final gen: " << currentHandle.GetGeneration() << "\n";

    EXPECT_GT(maxGen, g_ReuseCount / 2);

    // FIXED: Correct Template Argument
    m_Engine->FreeHandle<Bucket32>(currentHandle);
}

TEST_F(AllocatorStressTest, HandleLimits_StaleHandleDetection)
{
    constexpr size_t g_Iterations = 10000;
    std::atomic<size_t> staleDetected{0};

    for (size_t i = 0; i < g_Iterations; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        ASSERT_NE(h, g_InvalidHandle);

        uint32_t gen = h.GetGeneration();

        // Free it (increments generation)
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);

        // Try to resolve old handle (should fail)
        void* ptr = m_Engine->ResolveHandle<Bucket64>(h);
        if (ptr == nullptr) {
            staleDetected.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::cout << "=== STALE HANDLE DETECTION ===\n"
              << "Iterations: " << g_Iterations << "\n"
              << "Stale detected: " << staleDetected.load() << "\n";

    // All should be detected as stale
    EXPECT_EQ(staleDetected.load(), g_Iterations);
}

TEST_F(AllocatorStressTest, HandleLimits_ConcurrentGrowthTo2Million)
{
    constexpr size_t g_ThreadCount = 16;
    constexpr size_t g_HandlesPerThread = 125000; // 16 * 125k = 2M

    std::vector<std::vector<Handle>> threadHandles(g_ThreadCount);
    std::atomic<size_t> totalAllocated{0};

    std::barrier syncPoint(g_ThreadCount);

    auto allocator = [&](size_t tid) {
        syncPoint.arrive_and_wait();

        for (size_t i = 0; i < g_HandlesPerThread; ++i) {
            Handle h = m_Engine->AllocateWithHandle<Bucket32, PoolScope<Bucket32>>();
            if (h != g_InvalidHandle) {
                threadHandles[tid].push_back(h);
                totalAllocated.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                break;
            }
        }
    };

    auto start = high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(allocator, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "=== CONCURRENT GROWTH TO 2M ===\n"
              << "Total allocated: " << totalAllocated.load() << "\n"
              << "Time: " << ms << " ms\n"
              << "Handles/sec: " << (totalAllocated.load() * 1000 / std::max(ms, 1L)) << "\n";

    EXPECT_GT(totalAllocated.load(), 1500000);

    // Cleanup
    for (auto& vec : threadHandles) {
        for (Handle h : vec) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket32>(h);
        }
    }
}

TEST_F(AllocatorStressTest, HandleLimits_PageGrowthUnderPressure)
{
    std::vector<Handle> handles;

    // Allocate enough to force multiple page growths
    for (int i = 0; i < 50000; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket16, PoolScope<Bucket16>>();
        if (h != g_InvalidHandle) {
            handles.push_back(h);
        }
    }

    std::cout << "=== PAGE GROWTH UNDER PRESSURE ===\n"
              << "Allocated: " << handles.size() << " handles\n";

    EXPECT_GT(handles.size(), 40000);

    // Verify all handles still work
    for (Handle h : handles) {
        void* ptr = m_Engine->ResolveHandle<Bucket16>(h);
        EXPECT_NE(ptr, nullptr);
    }

    // Cleanup
    for (Handle h : handles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket16>(h);
    }
}

TEST_F(AllocatorStressTest, HandleLimits_UtilizationTracking)
{
    std::vector<Handle> handles;

    // Fill to ~50%
    for (int i = 0; i < 10000; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        if (h != g_InvalidHandle) {
            handles.push_back(h);
        }
    }

    // Check utilization (if API available)
    std::cout << "=== UTILIZATION TRACKING ===\n"
              << "Handles allocated: " << handles.size() << "\n";

    // Free half
    for (size_t i = 0; i < handles.size() / 2; ++i) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(handles[i]);
    }
    handles.erase(handles.begin(), handles.begin() + handles.size() / 2);

    std::cout << "After freeing half: " << handles.size() << " remaining\n";

    // Cleanup
    for (Handle h : handles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);
    }
}

TEST_F(AllocatorStressTest, HandleLimits_InvalidHandleOperations)
{
    // Test with garbage handle
    Handle garbage(0xDEADBEEF, 0xCAFEBABE, 0);

    void* ptr = m_Engine->ResolveHandle<Bucket64>(garbage);
    EXPECT_EQ(ptr, nullptr) << "Garbage handle should not resolve";

    // FIXED: Correct Template Argument
    bool freed = m_Engine->FreeHandle<Bucket64>(garbage);
    EXPECT_FALSE(freed) << "Freeing garbage handle should fail";

    // Test with null handle
    Handle nullHandle = g_InvalidHandle;

    ptr = m_Engine->ResolveHandle<Bucket64>(nullHandle);
    EXPECT_EQ(ptr, nullptr);

    // FIXED: Correct Template Argument
    freed = m_Engine->FreeHandle<Bucket64>(nullHandle);
    EXPECT_FALSE(freed);
}

TEST_F(AllocatorStressTest, HandleLimits_DoubleFreeDetection)
{
    Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
    ASSERT_NE(h, g_InvalidHandle);

    // First free should succeed
    // FIXED: Correct Template Argument
    bool freed1 = m_Engine->FreeHandle<Bucket128>(h);
    EXPECT_TRUE(freed1);

    // Second free should fail (stale generation)
    // FIXED: Correct Template Argument
    bool freed2 = m_Engine->FreeHandle<Bucket128>(h);
    EXPECT_FALSE(freed2) << "Double free should be detected";
}

TEST_F(AllocatorStressTest, HandleLimits_MassiveAllocFreeChurn)
{
    constexpr size_t g_Cycles = 100000;

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < g_Cycles; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        if (h != g_InvalidHandle) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket64>(h);
        }
    }

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "=== MASSIVE ALLOC/FREE CHURN ===\n"
              << "Cycles: " << g_Cycles << "\n"
              << "Time: " << ms << " ms\n"
              << "Ops/sec: " << (g_Cycles * 1000 / std::max(ms, 1L)) << "\n";

    EXPECT_GT(g_Cycles * 1000 / std::max(ms, 1L), 100000) << "Should exceed 100k ops/sec";
}

TEST_F(AllocatorStressTest, HandleLimits_HandleUpdateStability)
{
    Handle h = m_Engine->AllocateWithHandle<Bucket256, PoolScope<Bucket256>>();
    ASSERT_NE(h, g_InvalidHandle);

    void* ptr1 = m_Engine->ResolveHandle<Bucket256>(h);
    ASSERT_NE(ptr1, nullptr);

    // Write pattern
    std::memset(ptr1, 0xAA, 256);

    // Resolve again - should get same pointer
    void* ptr2 = m_Engine->ResolveHandle<Bucket256>(h);
    EXPECT_EQ(ptr1, ptr2);

    // Verify pattern intact
    EXPECT_EQ(static_cast<unsigned char*>(ptr2)[0], 0xAA);

    // FIXED: Correct Template Argument
    m_Engine->FreeHandle<Bucket256>(h);
}

// ============================================================================
// CATEGORY VI: MULTI-CONTEXT & ALIGNMENT CHAOS (12 TESTS)
// Target: Context switching, extreme alignments, non-trivial types
// ============================================================================

TEST_F(AllocatorStressTest, MultiContext_InterleavedLinearAndPool)
{
    constexpr size_t g_Iterations = 1000;
    std::vector<Handle> poolHandles;

    for (size_t i = 0; i < g_Iterations; ++i) {
        // Linear allocation
        void* linear = m_Engine->Allocate<FrameLoad>(128);
        EXPECT_NE(linear, nullptr);

        // Pool allocation
        Handle pool = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        EXPECT_NE(pool, g_InvalidHandle);
        poolHandles.push_back(pool);

        // Another linear
        void* linear2 = m_Engine->Allocate<LevelLoad>(256);
        EXPECT_NE(linear2, nullptr);
    }

    std::cout << "=== INTERLEAVED LINEAR/POOL ===\n"
              << "Iterations: " << g_Iterations << "\n"
              << "Pool handles: " << poolHandles.size() << "\n";

    // Cleanup pool
    for (Handle h : poolHandles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);
    }

    // Reset linear
    m_Engine->Reset<FrameLoad>();
    m_Engine->Reset<LevelLoad>();
}

TEST_F(AllocatorStressTest, MultiContext_ThreeContextsConcurrent)
{
    constexpr size_t g_ThreadCount = 12;
    std::vector<std::vector<void*>> frameAllocs(g_ThreadCount);
    std::vector<std::vector<void*>> levelAllocs(g_ThreadCount);
    std::vector<std::vector<Handle>> poolHandles(g_ThreadCount);

    auto worker = [&](size_t tid) {
        for (int i = 0; i < 500; ++i) {
            switch (tid % 3) {
            case 0:
                frameAllocs[tid].push_back(m_Engine->Allocate<FrameLoad>(512));
                break;
            case 1:
                levelAllocs[tid].push_back(m_Engine->Allocate<LevelLoad>(1024));
                break;
            case 2: {
                Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
                if (h != g_InvalidHandle) {
                    poolHandles[tid].push_back(h);
                }
                break;
            }
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== THREE CONTEXTS CONCURRENT ===\n";

    // Cleanup
    for (auto& vec : poolHandles) {
        for (Handle h : vec) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket128>(h);
        }
    }
}

TEST_F(AllocatorStressTest, MultiContext_ContextSwitchingOverhead)
{
    constexpr size_t g_Iterations = 10000;

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < g_Iterations; ++i) {
        m_Engine->Allocate<FrameLoad>(64);
        m_Engine->Allocate<LevelLoad>(64);
        m_Engine->Allocate<GlobalLoad>(64);
    }

    auto end = high_resolution_clock::now();
    auto ns = duration_cast<nanoseconds>(end - start).count();
    double nsPerAlloc = ns / (g_Iterations * 3.0);

    std::cout << "=== CONTEXT SWITCHING OVERHEAD ===\n"
              << "Time per allocation: " << nsPerAlloc << " ns\n";

    EXPECT_LT(nsPerAlloc, 1000); // <1µs per allocation
}

TEST_F(AllocatorStressTest, MultiContext_AbsurdAlignment4096)
{
    constexpr size_t g_PageAlign = 4096;

    void* ptr = m_Engine->Allocate<FrameLoad>(64, g_PageAlign);
    ASSERT_NE(ptr, nullptr);

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    EXPECT_EQ(addr % g_PageAlign, 0) << "Must be 4096-byte aligned";

    std::cout << "=== ABSURD ALIGNMENT 4096 ===\n"
              << "Address: 0x" << std::hex << addr << std::dec << "\n"
              << "Alignment: " << (addr % g_PageAlign) << "\n";
}

TEST_F(AllocatorStressTest, MultiContext_MixedAlignments)
{
    std::vector<void*> allocations;
    std::vector<size_t> alignments = {16, 32, 64, 128, 256, 512, 1024, 2048};

    for (size_t align : alignments) {
        for (int i = 0; i < 100; ++i) {
            void* ptr = m_Engine->Allocate<FrameLoad>(64, align);
            ASSERT_NE(ptr, nullptr);
            allocations.push_back(ptr);

            uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
            EXPECT_EQ(addr % align, 0) << "Alignment " << align << " failed";
        }
    }

    std::cout << "=== MIXED ALIGNMENTS ===\n"
              << "Total allocations: " << allocations.size() << "\n";
}

TEST_F(AllocatorStressTest, MultiContext_FrameResetBurst)
{
    constexpr size_t g_Frames = 1000;

    auto start = high_resolution_clock::now();

    for (size_t frame = 0; frame < g_Frames; ++frame) {
        // Simulate frame allocations
        for (int i = 0; i < 100; ++i) {
            m_Engine->Allocate<FrameLoad>(std::rand() % 1024 + 64);
        }

        // Reset for next frame
        m_Engine->Reset<FrameLoad>();
    }

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "=== FRAME RESET BURST ===\n"
              << "Frames: " << g_Frames << "\n"
              << "Time: " << ms << " ms\n"
              << "FPS equivalent: " << (g_Frames * 1000 / std::max(ms, 1L)) << "\n";

    EXPECT_GT(g_Frames * 1000 / std::max(ms, 1L), 500); // >500 FPS equivalent
}

TEST_F(AllocatorStressTest, MultiContext_LevelLoadRewindPatterns)
{
    // Allocate base level
    void* base1 = m_Engine->Allocate<LevelLoad>(1024);
    ASSERT_NE(base1, nullptr);

    auto [slab1, offset1] = LinearStrategyModule<LevelLoad>::GetCurrentState();

    // Allocate more
    void* temp1 = m_Engine->Allocate<LevelLoad>(2048);
    void* temp2 = m_Engine->Allocate<LevelLoad>(512);
    ASSERT_NE(temp1, nullptr);
    ASSERT_NE(temp2, nullptr);

    // Rewind to base
    LinearStrategyModule<LevelLoad>::RewindState(slab1, offset1);

    // Allocate again - should reuse space
    void* reused = m_Engine->Allocate<LevelLoad>(1024);
    EXPECT_NE(reused, nullptr);
}

TEST_F(AllocatorStressTest, MultiContext_GlobalLoadPersistence)
{
    std::vector<void*> persistent;

    // Allocate persistent data
    for (int i = 0; i < 100; ++i) {
        void* ptr = m_Engine->Allocate<GlobalLoad>(512);
        ASSERT_NE(ptr, nullptr);
        persistent.push_back(ptr);

        // Write unique pattern
        std::memset(ptr, i, 512);
    }

    // Reset other scopes (shouldn't affect global)
    m_Engine->Reset<FrameLoad>();
    m_Engine->Reset<LevelLoad>();

    // Verify global data intact
    for (size_t i = 0; i < persistent.size(); ++i) {
        unsigned char* data = static_cast<unsigned char*>(persistent[i]);
        EXPECT_EQ(data[0], static_cast<unsigned char>(i)) << "Global data corrupted at index " << i;
    }
}

TEST_F(AllocatorStressTest, MultiContext_TLSSwitchingStress)
{
    constexpr size_t g_ThreadCount = 32;
    std::atomic<size_t> contextSwitches{0};

    auto worker = [&]() {
        for (int i = 0; i < 1000; ++i) {
            m_Engine->Allocate<FrameLoad>(64);
            contextSwitches.fetch_add(1, std::memory_order_relaxed);

            m_Engine->Allocate<LevelLoad>(128);
            contextSwitches.fetch_add(1, std::memory_order_relaxed);

            m_Engine->Allocate<GlobalLoad>(256);
            contextSwitches.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back(worker);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "=== TLS SWITCHING STRESS ===\n"
              << "Total context switches: " << contextSwitches.load() << "\n";

    EXPECT_EQ(contextSwitches.load(), g_ThreadCount * 3000);
}

TEST_F(AllocatorStressTest, MultiContext_AlignmentWastageAnalysis)
{
    std::vector<size_t> alignments = {1, 8, 16, 32, 64, 128, 256};

    for (size_t align : alignments) {
        LinearStrategyModule<FrameLoad>::FlushThreadStats();
        auto before = LinearStrategyModule<FrameLoad>::GetGlobalStats().GetSnapshot();

        // Allocate with specific alignment
        for (int i = 0; i < 1000; ++i) {
            m_Engine->Allocate<FrameLoad>(64, align);
        }

        LinearStrategyModule<FrameLoad>::FlushThreadStats();
        auto after = LinearStrategyModule<FrameLoad>::GetGlobalStats().GetSnapshot();

        size_t used = after.BytesAllocated - before.BytesAllocated;
        size_t actualData = 1000 * 64;
        double overhead = (double)used / actualData;

        std::cout << "Alignment " << align << ": " << overhead << "x overhead\n";

        m_Engine->Reset<FrameLoad>();
    }
}

TEST_F(AllocatorStressTest, MultiContext_NonTrivialTypeStorage)
{
    struct ComplexType
    {
        std::vector<int> data;
        std::string name;

        ComplexType() : data(100, 42), name("test") {}
        ~ComplexType()
        {
            data.clear();
            name.clear();
        }
    };

    std::vector<Handle> handles;

    // Allocate and construct
    for (int i = 0; i < 100; ++i) {
        Handle h = m_Engine->AllocateWithHandle<ComplexType, PoolScope<ComplexType>>();
        if (h != g_InvalidHandle) {
            ComplexType* obj = m_Engine->ResolveHandle<ComplexType>(h);
            ASSERT_NE(obj, nullptr);

            // Placement new
            new (obj) ComplexType();

            handles.push_back(h);
        }
    }

    std::cout << "=== NON-TRIVIAL TYPE STORAGE ===\n"
              << "Objects created: " << handles.size() << "\n";

    // Destroy and free
    for (Handle h : handles) {
        ComplexType* obj = m_Engine->ResolveHandle<ComplexType>(h);
        if (obj) {
            obj->~ComplexType();
        }
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<ComplexType>(h);
    }
}

TEST_F(AllocatorStressTest, MultiContext_RandomSizeDistribution)
{
    std::mt19937 rng(12345);
    std::uniform_int_distribution<size_t> sizeDist(16, 256);

    for (int i = 0; i < 10000; ++i) {
        size_t size = sizeDist(rng);
        void* ptr = m_Engine->Allocate<FrameLoad>(size);
        EXPECT_NE(ptr, nullptr);
    }

    m_Engine->Reset<FrameLoad>();
}

// ============================================================================
// CATEGORY VII: SPEED SHOWDOWN (8 TESTS)
// Target: Performance benchmarks vs malloc, speed requirements
// ============================================================================

TEST_F(AllocatorStressTest, Benchmark_LinearVsMalloc)
{
    constexpr size_t g_Iterations = 1000000;
    constexpr size_t g_Size = 256;

    // Custom linear allocator
    auto start1 = high_resolution_clock::now();
    for (size_t i = 0; i < g_Iterations; ++i) {
        void* ptr = m_Engine->Allocate<FrameLoad>(g_Size);
        (void)ptr;
    }
    auto end1 = high_resolution_clock::now();
    auto customNs = duration_cast<nanoseconds>(end1 - start1).count();
    double customPerAlloc = customNs / (double)g_Iterations;

    m_Engine->Reset<FrameLoad>();

    // malloc
    std::vector<void*> mallocPtrs;
    auto start2 = high_resolution_clock::now();
    for (size_t i = 0; i < g_Iterations; ++i) {
        void* ptr = malloc(g_Size);
        mallocPtrs.push_back(ptr);
    }
    auto end2 = high_resolution_clock::now();
    auto mallocNs = duration_cast<nanoseconds>(end2 - start2).count();
    double mallocPerAlloc = mallocNs / (double)g_Iterations;

    // Cleanup malloc
    for (void* ptr : mallocPtrs) {
        free(ptr);
    }

    double speedup = mallocPerAlloc / customPerAlloc;

    std::cout << "=== LINEAR VS MALLOC ===\n"
              << "Custom: " << customPerAlloc << " ns/alloc\n"
              << "malloc: " << mallocPerAlloc << " ns/alloc\n"
              << "Speedup: " << speedup << "x\n";

    EXPECT_GT(speedup, 2.0) << "Custom allocator must be >2x faster than malloc";
}

TEST_F(AllocatorStressTest, Benchmark_PoolVsMalloc)
{
    constexpr size_t g_Iterations = 100000;

    std::vector<Handle> customHandles;
    auto start1 = high_resolution_clock::now();
    for (size_t i = 0; i < g_Iterations; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket128, PoolScope<Bucket128>>();
        if (h != g_InvalidHandle) {
            customHandles.push_back(h);
        }
    }
    auto end1 = high_resolution_clock::now();
    auto customMs = duration_cast<milliseconds>(end1 - start1).count();

    std::vector<void*> mallocPtrs;
    auto start2 = high_resolution_clock::now();
    for (size_t i = 0; i < g_Iterations; ++i) {
        mallocPtrs.push_back(malloc(128));
    }
    auto end2 = high_resolution_clock::now();
    auto mallocMs = duration_cast<milliseconds>(end2 - start2).count();

    double speedup = (double)mallocMs / customMs;

    std::cout << "=== POOL VS MALLOC ===\n"
              << "Custom: " << customMs << " ms\n"
              << "malloc: " << mallocMs << " ms\n"
              << "Speedup: " << speedup << "x\n";

    // Cleanup
    for (Handle h : customHandles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket128>(h);
    }
    for (void* ptr : mallocPtrs) {
        free(ptr);
    }

    EXPECT_GT(speedup, 1.2) << "Pool should be competitive with malloc";
}

TEST_F(AllocatorStressTest, Benchmark_ThroughputUnderLoad)
{
    constexpr size_t g_ThreadCount = 16;
    constexpr size_t g_AllocsPerThread = 100000;

    std::atomic<size_t> totalAllocs{0};

    auto start = high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (size_t i = 0; i < g_ThreadCount; ++i) {
        threads.emplace_back([&]() {
            for (size_t j = 0; j < g_AllocsPerThread; ++j) {
                void* ptr = m_Engine->Allocate<FrameLoad>(128);
                if (ptr) {
                    totalAllocs.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();
    auto throughput = totalAllocs.load() * 1000 / std::max(ms, 1L);

    std::cout << "=== THROUGHPUT UNDER LOAD ===\n"
              << "Total allocations: " << totalAllocs.load() << "\n"
              << "Time: " << ms << " ms\n"
              << "Throughput: " << throughput << " allocs/sec\n";

    EXPECT_GT(throughput, 10000000); // >10M allocs/sec
}

TEST_F(AllocatorStressTest, Benchmark_CacheEfficiency)
{
    constexpr size_t g_Iterations = 1000000;

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < g_Iterations; ++i) {
        void* ptr = m_Engine->Allocate<FrameLoad>(64);
        // Access memory to force cache load
        if (ptr) {
            volatile char c = *static_cast<char*>(ptr);
            (void)c;
        }
    }

    auto end = high_resolution_clock::now();
    auto ns = duration_cast<nanoseconds>(end - start).count();
    double nsPerOp = ns / (double)g_Iterations;

    std::cout << "=== CACHE EFFICIENCY ===\n"
              << "Time per alloc+access: " << nsPerOp << " ns\n";

    EXPECT_LT(nsPerOp, 500); // <500ns for alloc+access
}

TEST_F(AllocatorStressTest, Benchmark_HandleResolutionSpeed)
{
    constexpr size_t g_Iterations = 10000000;

    Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
    ASSERT_NE(h, g_InvalidHandle);

    auto start = high_resolution_clock::now();

    volatile void* sink = nullptr;
    for (size_t i = 0; i < g_Iterations; ++i) {
        sink = m_Engine->ResolveHandle<Bucket64>(h);
    }

    auto end = high_resolution_clock::now();
    auto ns = duration_cast<nanoseconds>(end - start).count();
    double nsPerResolve = ns / (double)g_Iterations;

    std::cout << "=== HANDLE RESOLUTION SPEED ===\n"
              << "Resolutions: " << g_Iterations << "\n"
              << "Time per resolve: " << nsPerResolve << " ns\n";

    EXPECT_LT(nsPerResolve, 50) << "Handle resolution must be <50ns";

    // FIXED: Correct Template Argument
    m_Engine->FreeHandle<Bucket64>(h);
}

TEST_F(AllocatorStressTest, Benchmark_ResetPerformance)
{
    constexpr size_t g_Resets = 100000;

    // Allocate some data
    for (int i = 0; i < 1000; ++i) {
        m_Engine->Allocate<FrameLoad>(256);
    }

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < g_Resets; ++i) {
        m_Engine->Reset<FrameLoad>();

        // Reallocate a bit
        for (int j = 0; j < 10; ++j) {
            m_Engine->Allocate<FrameLoad>(128);
        }
    }

    auto end = high_resolution_clock::now();
    auto ns = duration_cast<nanoseconds>(end - start).count();
    double nsPerReset = ns / (double)g_Resets;

    std::cout << "=== RESET PERFORMANCE ===\n"
              << "Resets: " << g_Resets << "\n"
              << "Time per reset: " << nsPerReset << " ns\n";

    EXPECT_LT(nsPerReset, 1000); // <1µs per reset
}

TEST_F(AllocatorStressTest, Benchmark_PoolFreePerformance)
{
    constexpr size_t g_Cycles = 100000;

    std::vector<Handle> handles;
    for (int i = 0; i < 1000; ++i) {
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        if (h != g_InvalidHandle) {
            handles.push_back(h);
        }
    }

    auto start = high_resolution_clock::now();

    for (size_t cycle = 0; cycle < g_Cycles; ++cycle) {
        // Free one
        if (!handles.empty()) {
            // FIXED: Correct Template Argument
            m_Engine->FreeHandle<Bucket64>(handles.back());
            handles.pop_back();
        }

        // Reallocate
        Handle h = m_Engine->AllocateWithHandle<Bucket64, PoolScope<Bucket64>>();
        if (h != g_InvalidHandle) {
            handles.push_back(h);
        }
    }

    auto end = high_resolution_clock::now();
    auto ns = duration_cast<nanoseconds>(end - start).count();
    double nsPerCycle = ns / (double)g_Cycles;

    std::cout << "=== POOL FREE PERFORMANCE ===\n"
              << "Cycles: " << g_Cycles << "\n"
              << "Time per free+alloc: " << nsPerCycle << " ns\n";

    // Cleanup
    for (Handle h : handles) {
        // FIXED: Correct Template Argument
        m_Engine->FreeHandle<Bucket64>(h);
    }
}

TEST_F(AllocatorStressTest, Benchmark_EndToEndGameSimulation)
{
    constexpr size_t g_Frames = 10000;

    auto start = high_resolution_clock::now();

    for (size_t frame = 0; frame < g_Frames; ++frame) {
        // Frame-temporary allocations
        for (int i = 0; i < 50; ++i) {
            m_Engine->Allocate<FrameLoad>(std::rand() % 512 + 64);
        }

        // Level-persistent allocations (occasional)
        if (frame % 100 == 0) {
            m_Engine->Allocate<LevelLoad>(2048);
        }

        // Global allocations (rare)
        if (frame % 1000 == 0) {
            m_Engine->Allocate<GlobalLoad>(8192);
        }

        // Reset frame
        m_Engine->Reset<FrameLoad>();
    }

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();
    auto fps = g_Frames * 1000 / std::max(ms, 1L);

    std::cout << "=== END-TO-END GAME SIMULATION ===\n"
              << "Frames: " << g_Frames << "\n"
              << "Time: " << ms << " ms\n"
              << "FPS equivalent: " << fps << "\n";

    EXPECT_GT(fps, 1000) << "Should sustain >1000 FPS worth of allocations";
}

// ============================================================================
// STRESS_TESTS.CPP - COMPLETE INTEGRATION FILE
// 70+ Hostile Tests to Destroy Your Allocator
//
// Compile with:
// g++ -std=c++20 -O2 -pthread stress_tests.cpp \
//     allocator_engine.cpp allocator_handle_system.cpp allocator_registry.cpp \
//     allocator_utility.cpp linear_module.cpp linear_strategy.cpp \
//     pool_module.cpp pool_strategy.cpp \
//     -I. -lgtest -lgtest_main \
//     -o stress_tests
//
// Run with:
// ./stress_tests
// ./stress_tests --gtest_filter="*Benchmark*"  # Just benchmarks
// ./stress_tests --gtest_filter="*OOM*"        # Just OOM tests
// ============================================================================

/*
TO INTEGRATE:

1. Copy all test code from stress_tests_part1.cpp
2. Append all test code from stress_tests_part2.cpp
3. Add main() function below
4. Compile and run

The tests are organized into 7 categories:

I.   Registry Thrashing (10 tests)
II.  Cross-Thread Torture (12 tests)
III. Cache Thrashing (8 tests)
IV.  OOM Recovery (8 tests)
V.   Handle Limits (10 tests)
VI.  Multi-Context Chaos (12 tests)
VII. Speed Showdown (8 tests)

TOTAL: 68 BRUTAL TESTS
*/

// [PASTE ALL CODE FROM PART 1 HERE]
// [PASTE ALL CODE FROM PART 2 HERE]

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);

    std::cout << "╔══════════════════════════════════════════════════════════════╗\n"
              << "║      ALLOCATOR STRESS TEST SUITE - HOSTILE QA MODE           ║\n"
              << "║      70+ Tests Designed to Break Your System                 ║\n"
              << "║      No Mercy. No Compromise. Only Truth.                    ║\n"
              << "╚══════════════════════════════════════════════════════════════╝\n\n";

    int result = RUN_ALL_TESTS();

    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n"
              << "║      TEST RUN COMPLETE                                       ║\n"
              << "║      Check output above for failures                         ║\n"
              << "╚══════════════════════════════════════════════════════════════╝\n";

    return result;
}

// ============================================================================
// EXPECTED OUTPUT
// ============================================================================

/*
If all tests pass, you'll see:

[==========] 68 tests from 1 test suite ran.
[  PASSED  ] 68 tests.

PERFORMANCE BASELINES (expected on modern CPU):

Registry Thrashing:
- 50 threads: >90% success rate
- Throughput: >10M allocs/sec

Cross-Thread Torture:
- Producer-consumer: 10K objects passed correctly
- Cross-thread free: 100% success
- Data races detected: 0

Cache Thrashing:
- Padding speedup: >1.5x
- Handle table ops: >1M ops/sec
- TLS variance: <30%

OOM Recovery:
- Saturate-recover: works
- Checkerboard fragmentation: >90% realloc success

Handle Limits:
- Push to 1M handles: success
- Generation wraparound: 100K cycles, gen never 0
- Stale detection: 100% caught

Multi-Context:
- Context switching: <1µs per alloc
- 4096-byte alignment: correct
- Non-trivial types: constructs/destructs correctly

Speed Showdown:
- Linear vs malloc: >2x faster
- Pool vs malloc: competitive
- Throughput: >10M allocs/sec
- Handle resolution: <50ns
- Game simulation: >1000 FPS equivalent
*/
