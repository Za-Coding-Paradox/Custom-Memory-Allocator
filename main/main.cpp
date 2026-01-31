#include <algorithm>
#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <modules/allocator_engine.h>
#include <random>
#include <thread>
#include <vector>

using namespace Allocator;

// ============================================================================
// 1. CUSTOM DATA STRUCTURES
// ============================================================================

// [Advanced] 64-byte aligned structure (Simulates AVX-512 data or Cache Lines)
struct alignas(64) HeavyAlignedMatrix {
  float Elements[16];
  char Padding[64];
};

// [Advanced] Large Object (~40KB) to force Slab Boundary checks
// Since our Slab Size is 64KB, two of these CANNOT fit in one slab.
struct BigChungus {
  uint8_t Data[40 * 1024];
};

// [Advanced] Game Entity for Handle Tests
struct GameEntity {
  uint64_t ID;
  double Pos[3];
  char Name[32];
  bool Active;
};

// ============================================================================
// 2. TEST FIXTURE
// ============================================================================

class AllocatorTest : public ::testing::Test {
protected:
  AllocatorEngine* Engine = nullptr;

  // Constants to make tests deterministic
  const size_t TEST_SLAB_SIZE = 65536;             // 64 KB
  const size_t TEST_ARENA_SIZE = 1024 * 1024 * 64; // 64 MB

  void SetUp() override {
    // Initialize engine with explicit sizes
    Engine = new AllocatorEngine(TEST_SLAB_SIZE, TEST_ARENA_SIZE);
    Engine->Initialize();
  }

  void TearDown() override {
    if (Engine) {
      Engine->Shutdown();
      delete Engine;
      Engine = nullptr;
    }
  }
};

// ============================================================================
// 3. BASELINE TESTS (Functionality Checks)
// ============================================================================

TEST_F(AllocatorTest, Basic_Allocation_And_Alignment) {
  // Allocates bytes and checks if they are valid
  void* p1 = Engine->Allocate<FrameLoad>(12);
  void* p2 = Engine->Allocate<FrameLoad>(128);
  void* p3 = Engine->Allocate<FrameLoad>(64);

  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  ASSERT_NE(p3, nullptr);

  uintptr_t u1 = reinterpret_cast<uintptr_t>(p1); // NOLINT
  uintptr_t u2 = reinterpret_cast<uintptr_t>(p2); // NOLINT

  // Check pointers are distinct
  EXPECT_NE(p1, p2);
  // Check basic 16-byte default alignment
  EXPECT_EQ(u1 % 16, 0);
  EXPECT_EQ(u2 % 16, 0);
}

TEST_F(AllocatorTest, Oversized_Allocation_Rejection) {
  // Request size larger than slab (64KB + 1KB)
  size_t tooBig = TEST_SLAB_SIZE + 1024;
  void* p = Engine->Allocate<FrameLoad>(tooBig);

  // Should return nullptr and log an error
  EXPECT_EQ(p, nullptr);
}

TEST_F(AllocatorTest, Zombie_Chain_Leak_Stress_Test) {
  // Allocate enough to create a chain of slabs, then reset.
  // 4KB * 50 = 200KB (Requires ~4 slabs of 64KB)
  const int numAllocations = 50;
  const size_t size = 4096;

  for (int i = 0; i < numAllocations; ++i) {
    void* p = Engine->Allocate<FrameLoad>(size);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAA, size); // Touch memory
  }

  // Resetting should reclaim the slabs internally
  Engine->Reset<FrameLoad>();

  // Do it again to ensure reuse works without crashing
  for (int i = 0; i < numAllocations; ++i) {
    void* p = Engine->Allocate<FrameLoad>(size);
    ASSERT_NE(p, nullptr);
  }
}

TEST_F(AllocatorTest, Handle_Lifecycle) {
  // Create
  auto handle = Engine->AllocateWithHandle<int, FrameLoad>();
  ASSERT_TRUE(handle.IsValid());

  // Resolve & Write
  int* ptr = Engine->ResolveHandle(handle);
  ASSERT_NE(ptr, nullptr);
  *ptr = 42;

  // Read back
  EXPECT_EQ(*Engine->ResolveHandle(handle), 42);

  // Free
  bool success = Engine->FreeHandle(handle);
  EXPECT_TRUE(success);

  // Validate (Should be invalid or resolve to nullptr)
  EXPECT_EQ(Engine->ResolveHandle(handle), nullptr);
}

TEST_F(AllocatorTest, Scoped_Marker_Rewind) {
  // Save State of LevelLoad
  auto checkpoint = Engine->SaveState<LevelLoad>();

  // FIX: Allocate on LevelLoad (so the pointer we saved gets advanced)
  int* temp = (int*)Engine->Allocate<LevelLoad>(sizeof(int));
  *temp = 999;
  uintptr_t addrOriginal = reinterpret_cast<uintptr_t>(temp);

  // Restore (Rewind) LevelLoad pointer back to checkpoint
  Engine->RestoreState<LevelLoad>(checkpoint.first, checkpoint.second);

  // FIX: Allocate on LevelLoad again (should now be back at the start)
  int* reused = (int*)Engine->Allocate<LevelLoad>(sizeof(int));
  uintptr_t addrNew = reinterpret_cast<uintptr_t>(reused);

  EXPECT_EQ(addrOriginal, addrNew);
}

TEST_F(AllocatorTest, Multithreaded_Allocation_Stress) {
  const int NUM_THREADS = 8;
  const int ALLOCS_PER_THREAD = 1000;

  std::vector<std::thread> threads;
  std::atomic<int> successCount{0};

  auto task = [&]() {
    for (int i = 0; i < ALLOCS_PER_THREAD; ++i) {
      size_t size = (i % 2 == 0) ? 256 : 32;
      void* p = Engine->Allocate<FrameLoad>(size);
      if (p)
        successCount++;
    }
  };

  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back(task);
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(successCount, NUM_THREADS * ALLOCS_PER_THREAD);
}

// ============================================================================
// 4. ADVANCED TESTS (Rigorous & Edge Cases)
// ============================================================================

TEST_F(AllocatorTest, Advanced_Alignment_Strictness_AVX) {
  // 1. Alloc Heavy Matrix
  auto p1 = Engine->Allocate<FrameLoad>(sizeof(HeavyAlignedMatrix), alignof(HeavyAlignedMatrix));

  // 2. Allocate 1 byte to throw off alignment
  std::cout << Engine->Allocate<FrameLoad>(1, 1);

  // 3. Alloc Another Heavy Matrix
  auto p2 = Engine->Allocate<FrameLoad>(sizeof(HeavyAlignedMatrix), alignof(HeavyAlignedMatrix));

  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);

  uintptr_t addr1 = reinterpret_cast<uintptr_t>(p1); // NOLINT
  uintptr_t addr2 = reinterpret_cast<uintptr_t>(p2); // NOLINT

  // Verify exactly 64-byte alignment
  EXPECT_EQ(addr1 % 64, 0) << "First matrix not 64-byte aligned";
  EXPECT_EQ(addr2 % 64, 0) << "Second matrix not 64-byte aligned";
}

TEST_F(AllocatorTest, Advanced_Slab_Boundary_Crossing) {
  // Alloc 1 (Fits in Slab 1)
  void* p1 = Engine->Allocate<FrameLoad>(sizeof(BigChungus));
  ASSERT_NE(p1, nullptr);

  // Alloc 2 (Must go to Slab 2 because 40KB + 40KB > 64KB)
  void* p2 = Engine->Allocate<FrameLoad>(sizeof(BigChungus));
  ASSERT_NE(p2, nullptr);

  uintptr_t u1 = reinterpret_cast<uintptr_t>(p1); // NOLINT
  uintptr_t u2 = reinterpret_cast<uintptr_t>(p2); // NOLINT

  // Pointers should NOT be contiguous.
  // If they were contiguous, diff would be sizeof(BigChungus).
  // Since they are in different malloc'd slabs, the gap is likely huge or random.
  bool contiguous = (u2 == u1 + sizeof(BigChungus));
  EXPECT_FALSE(contiguous) << "Allocator failed to switch slabs; memory overwrite likely!";
}

TEST_F(AllocatorTest, Advanced_Rewind_And_Overwrite) {
  // 1. Save LevelLoad
  auto ckpt = Engine->SaveState<LevelLoad>();

  // 2. Write Garbage (FIX: Use LevelLoad)
  struct Data {
    int val[10];
  };
  Data* garbage = (Data*)Engine->Allocate<LevelLoad>(sizeof(Data));
  garbage->val[0] = 0xDEADBEEF;
  uintptr_t addrGarbage = reinterpret_cast<uintptr_t>(garbage);

  // 3. Rewind LevelLoad
  Engine->RestoreState<LevelLoad>(ckpt.first, ckpt.second);

  // 4. Write Gold (FIX: Use LevelLoad)
  Data* gold = (Data*)Engine->Allocate<LevelLoad>(sizeof(Data));
  gold->val[0] = 0x11111111;
  uintptr_t addrGold = reinterpret_cast<uintptr_t>(gold);

  // 5. Verify
  EXPECT_EQ(addrGold, addrGarbage);    // Addresses should match
  EXPECT_EQ(gold->val[0], 0x11111111); // Data should be overwritten
}

TEST_F(AllocatorTest, Advanced_Handle_Data_Integrity) {
  auto handle = Engine->AllocateWithHandle<GameEntity, FrameLoad>();
  GameEntity* ent = Engine->ResolveHandle(handle);

  ent->ID = 101;
  ent->Pos[0] = 50.0;
  std::strcpy(ent->Name, "TestPlayer");

  // Resolve again to ensure table stability
  GameEntity* check = Engine->ResolveHandle(handle);
  EXPECT_EQ(check->ID, 101);
  EXPECT_STREQ(check->Name, "TestPlayer");
}

TEST_F(AllocatorTest, Advanced_Chaos_Monkey) {
  std::mt19937 rng(1337);
  // Mix of tiny allocations (header stress) and medium allocations
  std::uniform_int_distribution<size_t> dist(1, 4000);

  const int ITERATIONS = 1000;
  for (int i = 0; i < ITERATIONS; ++i) {
    size_t sz = dist(rng);
    void* p = Engine->Allocate<FrameLoad>(sz);
    ASSERT_NE(p, nullptr) << "Chaos Monkey failed at alloc " << i << " size " << sz;

    // Write guard bytes to check for immediate segfaults
    volatile uint8_t* bytes = (uint8_t*)p;
    bytes[0] = 0xFF;
    if (sz > 1)
      bytes[sz - 1] = 0xEE;
  }
}

// ============================================================================
// 5. MAIN ENTRY POINT
// ============================================================================

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
