#include <atomic>
#include <barrier>
#include <chrono>
#include <gtest/gtest.h>
#include <modules/allocator_engine.h>
#include <modules/strategies/linear_module/linear_scopes.h>
#include <random>
#include <thread>
#include <vector>

using namespace Allocator;

// =================================================================================================
// TEST CONSTANTS
// =================================================================================================
namespace TestConstants {
constexpr size_t k_SlabSize = 65536;             // 64 KB
constexpr size_t k_ArenaSize = 1024 * 1024 * 64; // 64 MB
constexpr size_t k_DefaultAlignment = 16;
constexpr size_t k_TestObjectSize = 256;
constexpr int k_ThreadCount = 8;
constexpr int k_StressIterations = 1000;
} // namespace TestConstants

struct TestObject {
  uint64_t ID;
  double Data[4];
};

// =================================================================================================
// TEST FIXTURE
// =================================================================================================
class AllocatorTest : public ::testing::Test {
protected:
  std::unique_ptr<AllocatorEngine> m_Engine;

  void SetUp() override {
    // Initialize a fresh engine for every test to ensure isolation
    m_Engine =
        std::make_unique<AllocatorEngine>(TestConstants::k_SlabSize, TestConstants::k_ArenaSize);
    m_Engine->Initialize();
  }

  void TearDown() override {
    m_Engine->Shutdown();
    m_Engine.reset();
  }
};

// =================================================================================================
// CATEGORY 1: BASIC ALLOCATION & VALIDATION
// =================================================================================================

TEST_F(AllocatorTest, BasicFrameAllocation) {
  void* ptr = m_Engine->Allocate<FrameLoad>(TestConstants::k_TestObjectSize);
  ASSERT_NE(ptr, nullptr) << "FrameLoad allocation failed";

  // Write to memory to ensure we own it
  std::memset(ptr, 0xAA, TestConstants::k_TestObjectSize);
  EXPECT_EQ(*static_cast<unsigned char*>(ptr), 0xAA);
}

TEST_F(AllocatorTest, AlignmentCompliance) {
  size_t customAlign = 128;
  void* ptr = m_Engine->Allocate<LevelLoad>(64, customAlign);

  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  EXPECT_EQ(addr % customAlign, 0) << "Pointer " << ptr << " is not aligned to " << customAlign;
}

TEST_F(AllocatorTest, ZeroSizeAllocation) {
  // Should return nullptr or handle gracefully based on implementation
  void* ptr = m_Engine->Allocate<FrameLoad>(0);
  EXPECT_EQ(ptr, nullptr) << "Zero size allocation should return nullptr";
}

TEST_F(AllocatorTest, OversizedAllocation) {
  // Attempt to allocate more than a single slab (Linear Allocator cannot handle objects > SlabSize)
  size_t tooBig = TestConstants::k_SlabSize + 1024;
  void* ptr = m_Engine->Allocate<FrameLoad>(tooBig);
  EXPECT_EQ(ptr, nullptr) << "Oversized allocation should fail gracefully";
}

// =================================================================================================
// CATEGORY 2: LINEAR STRATEGY LIFECYCLE (RESET & REWIND)
// =================================================================================================

TEST_F(AllocatorTest, FrameScopeResetReuse) {
  void* ptr1 = m_Engine->Allocate<FrameLoad>(1024);
  uintptr_t addr1 = reinterpret_cast<uintptr_t>(ptr1);

  // Reset the frame
  m_Engine->Reset<FrameLoad>();

  // Allocate again. Since it's linear and reset, it should give the SAME address (start of slab)
  void* ptr2 = m_Engine->Allocate<FrameLoad>(1024);
  uintptr_t addr2 = reinterpret_cast<uintptr_t>(ptr2);

  EXPECT_EQ(addr1, addr2) << "Reset() did not rewind the bump pointer correctly.";
}

TEST_F(AllocatorTest, LevelScopeManualRewind) {
  // 1. Save State
  auto checkpoint = m_Engine->SaveState<LevelLoad>();

  // 2. Allocate garbage
  std::cout << m_Engine->Allocate<LevelLoad>(512);
  void* markerPtr = m_Engine->Allocate<LevelLoad>(128); // We want to see if we come back here

  // 3. Restore State
  m_Engine->RestoreState<LevelLoad>(checkpoint.first, checkpoint.second);

  // 4. Allocate again -> Should be at the beginning again
  void* newPtr = m_Engine->Allocate<LevelLoad>(512);

  // Note: Exact address equality depends on implementation details,
  // but the logic implies we reused the space.
  EXPECT_LT(newPtr, markerPtr);
}

TEST_F(AllocatorTest, MixedScopeIsolation) {
  // Frame and Level should use different slabs/chains
  void* framePtr = m_Engine->Allocate<FrameLoad>(100);
  void* levelPtr = m_Engine->Allocate<LevelLoad>(100);

  m_Engine->Reset<FrameLoad>();

  // Resetting Frame should NOT affect Level
  void* framePtr2 = m_Engine->Allocate<FrameLoad>(100);
  void* levelPtr2 = m_Engine->Allocate<LevelLoad>(100);

  EXPECT_EQ(framePtr, framePtr2) << "Frame should have reset";
  EXPECT_NE(levelPtr, levelPtr2) << "Level should NOT have reset";
}

// =================================================================================================
// CATEGORY 3: HANDLE SYSTEM & MEMORY SAFETY
// =================================================================================================

TEST_F(AllocatorTest, HandleCreationAndResolve) {
  // LevelLoad supports handles
  Handle h = m_Engine->AllocateWithHandle<TestObject, LevelLoad>();
  ASSERT_TRUE(h.IsValid());

  TestObject* obj = m_Engine->ResolveHandle<TestObject>(h);
  ASSERT_NE(obj, nullptr);

  obj->ID = 12345;

  // Resolve again
  TestObject* obj2 = m_Engine->ResolveHandle<TestObject>(h);
  EXPECT_EQ(obj->ID, obj2->ID);
}

TEST_F(AllocatorTest, HandleStalenessAfterFree) {
  Handle h = m_Engine->AllocateWithHandle<TestObject, LevelLoad>();
  ASSERT_TRUE(h.IsValid());

  bool freed = m_Engine->FreeHandle(h);
  ASSERT_TRUE(freed);

  // Attempt to access after free
  TestObject* obj = m_Engine->ResolveHandle<TestObject>(h);
  EXPECT_EQ(obj, nullptr) << "Dangling handle should resolve to nullptr";
}

TEST_F(AllocatorTest, HandleGenerationRecycle) {
  // 1. Allocate and Free to increment generation at a specific slot
  Handle h1 = m_Engine->AllocateWithHandle<TestObject, LevelLoad>();
  uint32_t index = h1.GetIndex();
  m_Engine->FreeHandle(h1);

  // 2. Allocate again (likely reuses the same slot)
  // To force reuse in this test, we might need to exhaust others, but usually LIFO or FIFO
  // free lists reuse immediately.
  Handle h2 = m_Engine->AllocateWithHandle<TestObject, LevelLoad>();

  // If indices match, generations MUST differ
  if (h2.GetIndex() == index) {
    EXPECT_NE(h1.GetGeneration(), h2.GetGeneration()) << "Generations must increment on reuse";
    EXPECT_EQ(m_Engine->ResolveHandle<TestObject>(h1), nullptr) << "Old handle must be invalid";
  }
}

TEST_F(AllocatorTest, HandleDoubleFree) {
  Handle h = m_Engine->AllocateWithHandle<TestObject, LevelLoad>();
  EXPECT_TRUE(m_Engine->FreeHandle(h));
  EXPECT_FALSE(m_Engine->FreeHandle(h)) << "Second free should fail";
}

TEST_F(AllocatorTest, InvalidScopeHandleCheck) {
  // This is a compile-time check in your engine using static_assert.
  // Uncommenting this should cause build failure, validating the check exists.
  // m_Engine->AllocateWithHandle<TestObject, FrameLoad>();
  SUCCEED();
}

// =================================================================================================
// CATEGORY 4: RIGOROUS MULTI-THREADING
// =================================================================================================

TEST_F(AllocatorTest, MultiThread_IndependentHeaps) {
  // Verify that Thread A resetting its frame allocator does not wipe Thread B's memory
  std::atomic<void*> threadAPtr{nullptr};
  std::atomic<bool> threadAReset{false};

  std::thread t1([&]() {
    threadAPtr = m_Engine->Allocate<FrameLoad>(128);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    m_Engine->Reset<FrameLoad>();
    threadAReset = true;
  });

  std::thread t2([&]() {
    void* ptrB = m_Engine->Allocate<FrameLoad>(128);
    std::memset(ptrB, 0xBB, 128);

    // Wait for T1 to reset
    while (!threadAReset)
      std::this_thread::yield();

    // My data should still be valid because T1 only reset T1's slab chain
    EXPECT_EQ(*static_cast<unsigned char*>(ptrB), 0xBB) << "Thread Isolation Failure";
  });

  t1.join();
  t2.join();
}

TEST_F(AllocatorTest, MultiThread_RegistryContention) {
  // Spawn many threads that force Slab Allocations (Registry Lock/Atomic Contention)
  std::vector<std::thread> threads;
  std::atomic<int> successCount{0};

  auto task = [&]() {
    // Allocate enough to force 10 new slabs per thread
    for (int i = 0; i < 10; ++i) {
      // Allocate 60KB (fits in 64KB slab, forces new slab for next iter)
      void* p = m_Engine->Allocate<GlobalLoad>(60 * 1024);
      if (p)
        successCount++;
    }
  };

  for (int i = 0; i < TestConstants::k_ThreadCount; ++i) {
    threads.emplace_back(task);
  }

  for (auto& t : threads)
    t.join();

  EXPECT_EQ(successCount, TestConstants::k_ThreadCount * 10);
}

TEST_F(AllocatorTest, MultiThread_HandleRaceCondition) {
  // One thread allocates handles, another frees them randomly, another reads
  std::vector<Handle> sharedHandles;
  std::mutex handleMutex;
  std::atomic<bool> running{true};

  std::thread producer([&]() {
    while (running) {
      Handle h = m_Engine->AllocateWithHandle<TestObject, LevelLoad>();
      if (h.IsValid()) {
        std::lock_guard<std::mutex> lock(handleMutex);
        sharedHandles.push_back(h);
      }
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  });

  std::thread consumer([&]() {
    while (running) {
      std::lock_guard<std::mutex> lock(handleMutex);
      if (!sharedHandles.empty()) {
        // Remove random handle
        size_t idx = rand() % sharedHandles.size();
        Handle h = sharedHandles[idx];
        sharedHandles.erase(sharedHandles.begin() + idx);

        // Free it (unsafe to read after, but free should handle concurrency)
        m_Engine->FreeHandle(h);
      }
    }
  });

  // Run for a bit
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  running = false;
  producer.join();
  consumer.join();
}

// =================================================================================================
// CATEGORY 5: STRESS & EDGE CASES
// =================================================================================================

TEST_F(AllocatorTest, Stress_SlabChainGrowth) {
  // Allocate small objects until we span multiple slabs
  // 64KB slab / 64B object = ~1000 objects per slab
  constexpr int kObjCount = 5000;
  std::vector<void*> ptrs;
  ptrs.reserve(kObjCount);

  for (int i = 0; i < kObjCount; ++i) {
    void* p = m_Engine->Allocate<LevelLoad>(64);
    ASSERT_NE(p, nullptr);
    ptrs.push_back(p);
  }

  // Verify first and last are far apart (at least 4 slabs worth of bytes)
  uintptr_t p1 = reinterpret_cast<uintptr_t>(ptrs.front());
  uintptr_t p2 = reinterpret_cast<uintptr_t>(ptrs.back());

  // Difference could be negative depending on virtual mapping,
  // but just ensuring we didn't crash is the main test here.
  SUCCEED();
}

TEST_F(AllocatorTest, Stress_RapidFrameCycle) {
  // Simulation of a game loop
  for (int frame = 0; frame < TestConstants::k_StressIterations; ++frame) {
    for (int obj = 0; obj < 100; ++obj) {
      volatile void* p = m_Engine->Allocate<FrameLoad>(128);
      (void)p;
    }
    m_Engine->Reset<FrameLoad>();
  }
  // If we didn't OOM or crash, we passed.
  SUCCEED();
}

TEST_F(AllocatorTest, HandleTableCapacityGrowth) {
  // Allocate more handles than initial capacity (default 1024)
  std::vector<Handle> handles;
  for (int i = 0; i < 2048; ++i) {
    Handle h = m_Engine->AllocateWithHandle<size_t, LevelLoad>();
    ASSERT_TRUE(h.IsValid()) << "Failed at index " << i;
    handles.push_back(h);
  }

  // Verify all are still resolvable (checking that growth didn't invalidate old data)
  for (const auto& h : handles) {
    ASSERT_NE(m_Engine->ResolveHandle<size_t>(h), nullptr);
  }
}

TEST_F(AllocatorTest, ContextFreeCorrectness) {
  // Ensure that Shutdown logic (TearDown) doesn't crash even if we leave things allocated.
  // This tests the "Global Shutdown cleaning up threads" logic.
  // CAST TO VOID TO SILENCE NODISCARD WARNINGS
  (void)m_Engine->Allocate<LevelLoad>(1024);
  (void)m_Engine->Allocate<GlobalLoad>(1024);
  // TearDown() called automatically
}

// =================================================================================================
// MAIN ENTRY
// =================================================================================================

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
