// =============================================================================
//  benchmark.cpp  —  Custom Allocator vs malloc
//  Pure performance measurement, zero test framework dependency.
// =============================================================================
//
// ─────────────────────────────────────────────────────────────────────────────
//  STEP 1 — ADD TO CMakeLists.txt
// ─────────────────────────────────────────────────────────────────────────────
//
//  add_executable(allocator_bench benchmarks/benchmark.cpp)
//  target_include_directories(allocator_bench PRIVATE ${CMAKE_SOURCE_DIR}/include)
//  target_link_libraries(allocator_bench PRIVATE allocator)
//  target_compile_options(allocator_bench PRIVATE -O3 -DNDEBUG -march=native)
//
// ─────────────────────────────────────────────────────────────────────────────
//  STEP 2 — BUILD
// ─────────────────────────────────────────────────────────────────────────────
//
//  cd build/release && ninja allocator_bench
//
// ─────────────────────────────────────────────────────────────────────────────
//  STEP 3 — PREPARE THE SYSTEM (do this every time before benchmarking)
// ─────────────────────────────────────────────────────────────────────────────
//
//  # Lock CPU to max frequency to prevent turbo/throttle noise:
//  sudo cpupower frequency-set --governor performance
//
//  # Disable ASLR (reduces variance in pointer-chasing benchmarks):
//  echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
//
//  # Restore when done:
//  sudo cpupower frequency-set --governor powersave
//  echo 2 | sudo tee /proc/sys/kernel/randomize_va_space
//
// ─────────────────────────────────────────────────────────────────────────────
//  STEP 4 — BASIC RUN
// ─────────────────────────────────────────────────────────────────────────────
//
//  # Pin to a single physical core (core 0) to eliminate scheduler noise:
//  taskset -c 0 ./build/release/allocator_bench
//
//  # CSV output (import into Excel / Python / gnuplot):
//  taskset -c 0 ./build/release/allocator_bench --csv > results.csv
//
// ─────────────────────────────────────────────────────────────────────────────
//  STEP 5 — PERF: HARDWARE COUNTER ANALYSIS
// ─────────────────────────────────────────────────────────────────────────────
//
//  # High-level: cycles, instructions, IPC, cache misses, branch mispredicts:
//  perf stat -e cycles,instructions,cache-misses,cache-references,\
//               branch-misses,L1-dcache-load-misses,LLC-load-misses \
//       taskset -c 0 ./build/release/allocator_bench
//
//  # How to read the output:
//  #   instructions / cycles    → IPC. >2 is good. <1 means memory-bound.
//  #   cache-misses / cache-references → L1 miss rate. >5% is bad on hot paths.
//  #   LLC-load-misses          → L3 misses, ~200 cycles each. Should be low.
//  #   branch-misses            → mispredicts. Each costs ~15 cycles.
//
//  # Per-function flamegraph (shows which call sites burn the most cycles):
//  perf record -g --call-graph dwarf taskset -c 0 ./build/release/allocator_bench
//  perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
//  # Open flame.svg in browser. Wide bars = hot code.
//
//  # Annotated source (see cycle counts per line of source):
//  perf record -g taskset -c 0 ./build/release/allocator_bench
//  perf report --stdio --no-children
//
// ─────────────────────────────────────────────────────────────────────────────
//  STEP 6 — VALGRIND CACHEGRIND (simulated cache hierarchy, no root needed)
// ─────────────────────────────────────────────────────────────────────────────
//
//  valgrind --tool=cachegrind --branch-sim=yes \
//           --cachegrind-out-file=cg.out \
//           ./build/release/allocator_bench
//
//  # Annotated summary sorted by LLC misses:
//  cg_annotate cg.out --sort=DLmr
//
//  # Diff two runs (before vs after a change):
//  cg_diff cg.before.out cg.after.out
//
//  # Column meanings:
//  #   Ir   = instruction reads
//  #   Dr/Dw = data reads/writes    D1mr = L1 miss    DLmr = LLC miss
//  #   Bc   = conditional branches  Bcm  = mispredicts
//
//  # Note: cachegrind runs ~40x slower. Use a reduced iteration count or
//  # run on a single benchmark section.
//
// ─────────────────────────────────────────────────────────────────────────────
//  STEP 7 — HEAPTRACK (heap profile over time, better than massif)
// ─────────────────────────────────────────────────────────────────────────────
//
//  sudo apt install heaptrack heaptrack-gui
//  heaptrack ./build/release/allocator_bench
//  heaptrack_gui heaptrack.allocator_bench.*.zst
//
//  # Shows: peak heap usage, allocation call stacks, temporal fragmentation.
//  # Useful for seeing how much the custom allocator actually reduces OS calls.
//
// ─────────────────────────────────────────────────────────────────────────────
//  STEP 8 — MULTIPLE TRIALS + MEDIAN (recommended for CI / regression tracking)
// ─────────────────────────────────────────────────────────────────────────────
//
//  # Run 7 times, collect all CSV rows, compute per-benchmark median:
//  for i in $(seq 1 7); do
//      taskset -c 0 ./build/release/allocator_bench --csv
//  done > all_runs.csv
//
//  python3 << 'EOF'
//  import csv, collections, statistics
//  rows = list(csv.DictReader(open('all_runs.csv')))
//  groups = collections.defaultdict(list)
//  for r in rows:
//      groups[r['benchmark']].append(float(r['custom_ns_per_op']))
//  for name, vals in sorted(groups.items()):
//      med = statistics.median(vals)
//      print(f"{name:45s}  median={med:7.2f} ns/op")
//  EOF
//
// ─────────────────────────────────────────────────────────────────────────────
//  OUTPUT COLUMN GUIDE
// ─────────────────────────────────────────────────────────────────────────────
//  ns/op    — nanoseconds per single operation (lower = faster)
//  speedup  — malloc_ns / custom_ns  (>1.0x means custom is faster)
//  verdict  — FASTER (>10% faster)  |  PAR (within 10%)  |  SLOWER
//
// =============================================================================

#include <modules/allocator_engine.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace Allocator;
using Clock = std::chrono::steady_clock;

// =============================================================================
//  POOL TARGET STRUCTS  (identical to tests.cpp for consistency)
// =============================================================================

struct Bucket16  { char data[16];  };
struct Bucket32  { char data[32];  };
struct Bucket64  { char data[64];  };
struct Bucket128 { char data[128]; };
struct Bucket256 { char data[256]; };

// =============================================================================
//  GLOBALS
// =============================================================================

static constexpr size_t kSlabSize  = 64ULL  * 1024;         // 64 KB
static constexpr size_t kArenaSize = 128ULL * 1024 * 1024; // 128 MB
static constexpr int    kRuns      = 7;   // median of this many runs per bench
static bool             g_CSV      = false;

// =============================================================================
//  TIMING HELPERS
// =============================================================================

static inline int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

template <typename Fn>
static int64_t MedianNs(Fn&& fn)
{
    int64_t s[kRuns];
    for (int i = 0; i < kRuns; ++i) {
        int64_t t0 = NowNs(); fn(); s[i] = NowNs() - t0;
    }
    std::sort(s, s + kRuns);
    return s[kRuns / 2];
}

// Prevent optimizer from eliding the allocation.
static inline void Sink(void* p) { asm volatile("" : : "r,m"(p) : "memory"); }

// =============================================================================
//  RESULT TABLE
// =============================================================================

struct BenchResult
{
    std::string domain;
    std::string name;
    int64_t     customNs;
    int64_t     mallocNs;
    size_t      ops;
    std::string notes;
};

static std::vector<BenchResult> g_Results;

static void Record(const char* domain, const char* name,
                   int64_t cNs, int64_t mNs, size_t ops, const char* notes = "")
{
    g_Results.push_back({domain, name, cNs, mNs, ops, notes});
}

static void PrintTable()
{
    const int W0=20, W1=34, W2=13, W3=13, W4=9, W5=10;
    const int total = W0+W1+W2+W3+W4+W5+16;
    std::string sep(total, '-');

    std::cout << "\n" << sep << "\n"
              << std::left
              << std::setw(W0) << "DOMAIN"
              << std::setw(W1) << "BENCHMARK"
              << std::setw(W2) << "CUSTOM"
              << std::setw(W3) << "MALLOC"
              << std::setw(W4) << "SPEEDUP"
              << std::setw(W5) << "VERDICT"
              << "NOTES\n"
              << sep << "\n";

    std::string lastDomain;
    for (const auto& r : g_Results) {
        if (r.domain != lastDomain) {
            if (!lastDomain.empty()) std::cout << "\n";
            lastDomain = r.domain;
        }
        double cOp = (double)r.customNs / (double)r.ops;
        double mOp = (double)r.mallocNs / (double)r.ops;
        double spd = (cOp > 0.0) ? mOp / cOp : 0.0;
        const char* verdict = spd >= 1.10 ? "FASTER" : spd >= 0.90 ? "PAR" : "SLOWER";

        char cs[24], ms[24], ss[16];
        snprintf(cs, sizeof(cs), "%.2f ns/op", cOp);
        snprintf(ms, sizeof(ms), "%.2f ns/op", mOp);
        snprintf(ss, sizeof(ss), "%.2fx", spd);

        std::cout << std::left
                  << std::setw(W0) << r.domain
                  << std::setw(W1) << r.name
                  << std::setw(W2) << cs
                  << std::setw(W3) << ms
                  << std::setw(W4) << ss
                  << std::setw(W5) << verdict
                  << r.notes << "\n";
    }
    std::cout << sep << "\n\n";
}

static void PrintCSV()
{
    std::cout << "domain,benchmark,custom_ns_per_op,malloc_ns_per_op,"
                 "speedup,verdict,total_ops,notes\n";
    for (const auto& r : g_Results) {
        double cOp = (double)r.customNs / (double)r.ops;
        double mOp = (double)r.mallocNs / (double)r.ops;
        double spd = (cOp > 0.0) ? mOp / cOp : 0.0;
        const char* v = spd >= 1.10 ? "FASTER" : spd >= 0.90 ? "PAR" : "SLOWER";
        printf("%s,%s,%.3f,%.3f,%.3f,%s,%zu,%s\n",
               r.domain.c_str(), r.name.c_str(),
               cOp, mOp, spd, v, r.ops, r.notes.c_str());
    }
}

// =============================================================================
//  I. THROUGHPUT
//  Measures the single fastest allocation path in isolation.
//  Linear = pure bump pointer (no free needed).
//  Pool   = lock-free CAS freelist pop.
//  malloc = tcmalloc / jemalloc thread-cache.
// =============================================================================

static void BenchThroughput()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t N = 200'000;

    // ── Linear 64B ───────────────────────────────────────────────────────
    int64_t cT = MedianNs([&]{
        for (size_t i = 0; i < N; ++i) Sink(eng.Allocate<FrameLoad>(64));
        eng.Reset<FrameLoad>();
    });
    int64_t mT = MedianNs([&]{
        std::vector<void*> p(N);
        for (size_t i = 0; i < N; ++i) p[i] = malloc(64);
        for (auto x : p) free(x);
    });
    Record("I. Throughput", "Linear alloc 64B",  cT, mT, N, "no free, O(1) bump");

    // ── Linear 512B ──────────────────────────────────────────────────────
    cT = MedianNs([&]{
        for (size_t i = 0; i < N; ++i) Sink(eng.Allocate<FrameLoad>(512));
        eng.Reset<FrameLoad>();
    });
    mT = MedianNs([&]{
        std::vector<void*> p(N);
        for (size_t i = 0; i < N; ++i) p[i] = malloc(512);
        for (auto x : p) free(x);
    });
    Record("I. Throughput", "Linear alloc 512B", cT, mT, N, "no free, O(1) bump");

    // ── Pool 64B alloc+free ───────────────────────────────────────────────
    std::vector<Handle> h(N);
    cT = MedianNs([&]{
        for (size_t i = 0; i < N; ++i) h[i] = eng.AllocateWithHandle<Bucket64>();
        for (size_t i = 0; i < N; ++i) eng.FreeHandle<Bucket64>(h[i]);
    });
    mT = MedianNs([&]{
        std::vector<void*> p(N);
        for (size_t i = 0; i < N; ++i) p[i] = malloc(64);
        for (auto x : p) free(x);
    });
    Record("I. Throughput", "Pool alloc+free 64B",  cT, mT, N*2, "full cycle, CAS freelist");

    // ── Pool 256B alloc+free ──────────────────────────────────────────────
    cT = MedianNs([&]{
        for (size_t i = 0; i < N; ++i) h[i] = eng.AllocateWithHandle<Bucket256>();
        for (size_t i = 0; i < N; ++i) eng.FreeHandle<Bucket256>(h[i]);
    });
    mT = MedianNs([&]{
        std::vector<void*> p(N);
        for (size_t i = 0; i < N; ++i) p[i] = malloc(256);
        for (auto x : p) free(x);
    });
    Record("I. Throughput", "Pool alloc+free 256B", cT, mT, N*2, "full cycle, CAS freelist");
}

// =============================================================================
//  II. ALLOC/FREE CHURN
//  Allocate a live set, then repeatedly free half and reallocate.
//  Tests freelist recycling throughput and metadata overhead over time.
// =============================================================================

static void BenchChurn()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t kSlots  = 4096;
    constexpr size_t kRounds = 100;
    constexpr size_t kOps    = kSlots + (kSlots/2) * kRounds * 2;

    // ── 128B ─────────────────────────────────────────────────────────────
    int64_t cT = MedianNs([&]{
        std::vector<Handle> live(kSlots);
        for (size_t i = 0; i < kSlots; ++i)
            live[i] = eng.AllocateWithHandle<Bucket128>();
        for (size_t r = 0; r < kRounds; ++r) {
            for (size_t i = 0; i < kSlots; i += 2) eng.FreeHandle<Bucket128>(live[i]);
            for (size_t i = 0; i < kSlots; i += 2) live[i] = eng.AllocateWithHandle<Bucket128>();
        }
        for (auto hh : live) eng.FreeHandle<Bucket128>(hh);
    });
    int64_t mT = MedianNs([&]{
        std::vector<void*> live(kSlots);
        for (size_t i = 0; i < kSlots; ++i) live[i] = malloc(128);
        for (size_t r = 0; r < kRounds; ++r)
            for (size_t i = 0; i < kSlots; i += 2) { free(live[i]); live[i] = malloc(128); }
        for (auto p : live) free(p);
    });
    Record("II. Churn", "Pool 128B 50% churn", cT, mT, kOps, "checkerboard free/realloc");

    // ── 32B ──────────────────────────────────────────────────────────────
    cT = MedianNs([&]{
        std::vector<Handle> live(kSlots);
        for (size_t i = 0; i < kSlots; ++i)
            live[i] = eng.AllocateWithHandle<Bucket32>();
        for (size_t r = 0; r < kRounds; ++r) {
            for (size_t i = 0; i < kSlots; i += 2) eng.FreeHandle<Bucket32>(live[i]);
            for (size_t i = 0; i < kSlots; i += 2) live[i] = eng.AllocateWithHandle<Bucket32>();
        }
        for (auto hh : live) eng.FreeHandle<Bucket32>(hh);
    });
    mT = MedianNs([&]{
        std::vector<void*> live(kSlots);
        for (size_t i = 0; i < kSlots; ++i) live[i] = malloc(32);
        for (size_t r = 0; r < kRounds; ++r)
            for (size_t i = 0; i < kSlots; i += 2) { free(live[i]); live[i] = malloc(32); }
        for (auto p : live) free(p);
    });
    Record("II. Churn", "Pool 32B 50% churn",  cT, mT, kOps, "small bucket recycling");
}

// =============================================================================
//  III. SIZE SWEEP
//  Sweeps allocation sizes from 16B to 16KB to find where the custom
//  allocator's advantage grows or collapses relative to malloc.
// =============================================================================

static void BenchSizeSweep()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t N = 100'000;
    const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};

    for (size_t sz : sizes) {
        int64_t cT = MedianNs([&]{
            for (size_t i = 0; i < N; ++i) Sink(eng.Allocate<FrameLoad>(sz));
            eng.Reset<FrameLoad>();
        });
        int64_t mT = MedianNs([&]{
            std::vector<void*> p(N);
            for (size_t i = 0; i < N; ++i) p[i] = malloc(sz);
            for (auto x : p) free(x);
        });
        char name[32], notes[32];
        snprintf(name,  sizeof(name),  "Linear %zuB", sz);
        snprintf(notes, sizeof(notes), "size=%zu", sz);
        Record("III. Size Sweep", name, cT, mT, N, notes);
    }
}

// =============================================================================
//  IV. CACHE COHERENCY
//  Allocates objects and then reads them back in different access patterns.
//  Linear allocator packs objects contiguously → prefetcher works perfectly.
//  malloc scatters objects across heap pages → cold misses on first access.
//
//  Sub-tests:
//    Sequential scan — best case for hardware prefetcher
//    Strided scan    — stresses TLB and L1 prefetch distance
//    Pool read       — measures handle-table indirection overhead
// =============================================================================

static void BenchCacheCoherency()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t kObjs   = 16384;
    constexpr size_t kPasses = 8;

    // ── Sequential scan ───────────────────────────────────────────────────
    {
        std::vector<void*> cp(kObjs), mp(kObjs);
        for (size_t i = 0; i < kObjs; ++i) {
            cp[i] = eng.Allocate<LevelLoad>(64);
            if (cp[i]) static_cast<volatile char*>(cp[i])[0] = (char)i;
            mp[i] = malloc(64);
            if (mp[i]) static_cast<volatile char*>(mp[i])[0] = (char)i;
        }
        int64_t cT = MedianNs([&]{
            volatile size_t s = 0;
            for (size_t p = 0; p < kPasses; ++p)
                for (size_t i = 0; i < kObjs; ++i) s += *static_cast<char*>(cp[i]);
            Sink((void*)&s);
        });
        int64_t mT = MedianNs([&]{
            volatile size_t s = 0;
            for (size_t p = 0; p < kPasses; ++p)
                for (size_t i = 0; i < kObjs; ++i) s += *static_cast<char*>(mp[i]);
            Sink((void*)&s);
        });
        Record("IV. Cache", "Seq scan 64B x16K", cT, mT,
               kObjs*kPasses, "contiguous slab vs scattered heap");
        eng.Reset<LevelLoad>();
        for (auto p : mp) free(p);
    }

    // ── Strided scan (stride = 8 objects) ────────────────────────────────
    {
        constexpr size_t kStride = 8;
        std::vector<void*> cp(kObjs), mp(kObjs);
        for (size_t i = 0; i < kObjs; ++i) {
            cp[i] = eng.Allocate<LevelLoad>(64);
            if (cp[i]) static_cast<volatile char*>(cp[i])[0] = (char)i;
            mp[i] = malloc(64);
            if (mp[i]) static_cast<volatile char*>(mp[i])[0] = (char)i;
        }
        int64_t cT = MedianNs([&]{
            volatile size_t s = 0;
            for (size_t p = 0; p < kPasses; ++p)
                for (size_t i = 0; i < kObjs; i += kStride) s += *static_cast<char*>(cp[i]);
            Sink((void*)&s);
        });
        int64_t mT = MedianNs([&]{
            volatile size_t s = 0;
            for (size_t p = 0; p < kPasses; ++p)
                for (size_t i = 0; i < kObjs; i += kStride) s += *static_cast<char*>(mp[i]);
            Sink((void*)&s);
        });
        Record("IV. Cache", "Strided scan stride=8", cT, mT,
               (kObjs/kStride)*kPasses, "probes TLB + prefetch distance");
        eng.Reset<LevelLoad>();
        for (auto p : mp) free(p);
    }

    // ── Pool read (handle resolve + access) ───────────────────────────────
    {
        constexpr size_t kPoolN = 4096;
        std::vector<Handle> handles(kPoolN);
        std::vector<void*>  mp(kPoolN);
        for (size_t i = 0; i < kPoolN; ++i) {
            handles[i] = eng.AllocateWithHandle<Bucket64>();
            Bucket64* obj = eng.ResolveHandle<Bucket64>(handles[i]);
            if (obj) obj->data[0] = (char)i;
            mp[i] = malloc(64);
            static_cast<char*>(mp[i])[0] = (char)i;
        }
        int64_t cT = MedianNs([&]{
            volatile size_t s = 0;
            for (size_t p = 0; p < kPasses; ++p)
                for (size_t i = 0; i < kPoolN; ++i) {
                    Bucket64* obj = eng.ResolveHandle<Bucket64>(handles[i]);
                    if (obj) s += obj->data[0];
                }
            Sink((void*)&s);
        });
        int64_t mT = MedianNs([&]{
            volatile size_t s = 0;
            for (size_t p = 0; p < kPasses; ++p)
                for (size_t i = 0; i < kPoolN; ++i) s += *static_cast<char*>(mp[i]);
            Sink((void*)&s);
        });
        Record("IV. Cache", "Pool resolve+read 64B", cT, mT,
               kPoolN*kPasses, "handle table seqlock overhead");
        for (size_t i = 0; i < kPoolN; ++i) { eng.FreeHandle<Bucket64>(handles[i]); free(mp[i]); }
    }
}

// =============================================================================
//  V. FRAGMENTATION
//  Worst-case fragmentation pattern: allocate N objects, free every other
//  one, then reallocate the same size.  Pool freelists recycle exact-fit
//  slots immediately.  malloc coalesces based on heap strategy.
// =============================================================================

static void BenchFragmentation()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t kSlots  = 2048;
    constexpr size_t kCycles = 50;

    // ── Same-size 128B ────────────────────────────────────────────────────
    int64_t cT = MedianNs([&]{
        std::vector<Handle> live(kSlots);
        for (size_t i = 0; i < kSlots; ++i) live[i] = eng.AllocateWithHandle<Bucket128>();
        for (size_t c = 0; c < kCycles; ++c) {
            for (size_t i = 0; i < kSlots; i += 2) eng.FreeHandle<Bucket128>(live[i]);
            for (size_t i = 0; i < kSlots; i += 2) live[i] = eng.AllocateWithHandle<Bucket128>();
        }
        for (auto hh : live) eng.FreeHandle<Bucket128>(hh);
    });
    int64_t mT = MedianNs([&]{
        std::vector<void*> live(kSlots);
        for (size_t i = 0; i < kSlots; ++i) live[i] = malloc(128);
        for (size_t c = 0; c < kCycles; ++c)
            for (size_t i = 0; i < kSlots; i += 2) { free(live[i]); live[i] = malloc(128); }
        for (auto p : live) free(p);
    });
    Record("V. Fragmentation", "Same-size 128B checker",
           cT, mT, kSlots*kCycles, "50% free+realloc per cycle");

    // ── Mixed-size 32B + 256B interleaved ────────────────────────────────
    // Pool: completely separate freelists per bucket.
    // malloc: 32B and 256B fragments can intermix and impede coalescing.
    constexpr size_t kMix = 1024;
    int64_t cT2 = MedianNs([&]{
        std::vector<Handle> s(kMix), l(kMix);
        for (size_t i = 0; i < kMix; ++i) {
            s[i] = eng.AllocateWithHandle<Bucket32>();
            l[i] = eng.AllocateWithHandle<Bucket256>();
        }
        for (size_t c = 0; c < kCycles; ++c)
            for (size_t i = 0; i < kMix; i += 2) {
                eng.FreeHandle<Bucket32>(s[i]);   s[i] = eng.AllocateWithHandle<Bucket32>();
                eng.FreeHandle<Bucket256>(l[i]);  l[i] = eng.AllocateWithHandle<Bucket256>();
            }
        for (size_t i = 0; i < kMix; ++i) { eng.FreeHandle<Bucket32>(s[i]); eng.FreeHandle<Bucket256>(l[i]); }
    });
    int64_t mT2 = MedianNs([&]{
        std::vector<void*> s(kMix), l(kMix);
        for (size_t i = 0; i < kMix; ++i) { s[i] = malloc(32); l[i] = malloc(256); }
        for (size_t c = 0; c < kCycles; ++c)
            for (size_t i = 0; i < kMix; i += 2) {
                free(s[i]); s[i] = malloc(32);
                free(l[i]); l[i] = malloc(256);
            }
        for (size_t i = 0; i < kMix; ++i) { free(s[i]); free(l[i]); }
    });
    Record("V. Fragmentation", "Mixed 32B+256B interleaved",
           cT2, mT2, kMix*2*kCycles, "separate buckets vs heap coalesce");
}

// =============================================================================
//  VI. CONCURRENCY
//  N threads allocate and free their own batches simultaneously.
//  Measures: CAS contention on pool freelist, slab growth mutex,
//  TLS isolation, vs malloc's per-thread cache contention.
// =============================================================================

static void BenchConcurrency()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t kPerThread = 20'000;
    const size_t threadCounts[] = {2, 4, 8, 16};

    for (size_t nT : threadCounts) {
        int64_t cT = MedianNs([&]{
            std::barrier go(static_cast<std::ptrdiff_t>(nT));
            std::vector<std::thread> threads;
            threads.reserve(nT);
            for (size_t t = 0; t < nT; ++t)
                threads.emplace_back([&]{
                    go.arrive_and_wait();
                    std::vector<Handle> hv; hv.reserve(kPerThread);
                    for (size_t i = 0; i < kPerThread; ++i)
                        hv.push_back(eng.AllocateWithHandle<Bucket128>());
                    for (auto hh : hv) eng.FreeHandle<Bucket128>(hh);
                });
            for (auto& t : threads) t.join();
        });
        int64_t mT = MedianNs([&]{
            std::barrier go(static_cast<std::ptrdiff_t>(nT));
            std::vector<std::thread> threads;
            threads.reserve(nT);
            for (size_t t = 0; t < nT; ++t)
                threads.emplace_back([&]{
                    go.arrive_and_wait();
                    std::vector<void*> pv; pv.reserve(kPerThread);
                    for (size_t i = 0; i < kPerThread; ++i) pv.push_back(malloc(128));
                    for (auto p : pv) free(p);
                });
            for (auto& t : threads) t.join();
        });
        char name[48], notes[32];
        snprintf(name,  sizeof(name),  "Pool 128B %zu threads", nT);
        snprintf(notes, sizeof(notes), "%zu threads x %zu ops", nT, kPerThread);
        Record("VI. Concurrency", name, cT, mT, kPerThread*nT*2, notes);
    }
}

// =============================================================================
//  VII. RESET vs FREE
//  The linear allocator's defining advantage.
//  A game frame allocates N scratch objects, then discards all of them.
//  Reset() rewinds the bump pointer in O(1).
//  malloc requires N individual free() calls.
// =============================================================================

static void BenchReset()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t kAllocsPerFrame = 10'000;
    constexpr size_t kFrames         = 1'000;

    // Pre-warm: acquire slab chain so GrowSlabChain is not timed.
    for (size_t i = 0; i < kAllocsPerFrame; ++i) eng.Allocate<FrameLoad>(128);
    eng.Reset<FrameLoad>();

    // ── FrameLoad: bump alloc N + Reset ──────────────────────────────────
    int64_t cT = MedianNs([&]{
        for (size_t f = 0; f < kFrames; ++f) {
            for (size_t i = 0; i < kAllocsPerFrame; ++i) Sink(eng.Allocate<FrameLoad>(128));
            eng.Reset<FrameLoad>();
        }
    });
    int64_t mT = MedianNs([&]{
        std::vector<void*> buf(kAllocsPerFrame);
        for (size_t f = 0; f < kFrames; ++f) {
            for (size_t i = 0; i < kAllocsPerFrame; ++i) buf[i] = malloc(128);
            for (auto p : buf) free(p);
        }
    });
    Record("VII. Reset", "Frame alloc+Reset 1K frames",
           cT, mT, kAllocsPerFrame*kFrames, "Reset=O(1) vs N x free()");

    // ── LevelLoad: GetCurrentState + RewindState ──────────────────────────
    constexpr size_t kSubAllocs = 5'000;
    for (size_t i = 0; i < kSubAllocs; ++i) eng.Allocate<LevelLoad>(256);
    eng.Reset<LevelLoad>();

    int64_t cT2 = MedianNs([&]{
        for (size_t f = 0; f < kFrames; ++f) {
            auto [slab, off] = LinearStrategyModule<LevelLoad>::GetCurrentState();
            for (size_t i = 0; i < kSubAllocs; ++i) Sink(eng.Allocate<LevelLoad>(256));
            LinearStrategyModule<LevelLoad>::RewindState(slab, off);
        }
    });
    int64_t mT2 = MedianNs([&]{
        std::vector<void*> buf(kSubAllocs);
        for (size_t f = 0; f < kFrames; ++f) {
            for (size_t i = 0; i < kSubAllocs; ++i) buf[i] = malloc(256);
            for (auto p : buf) free(p);
        }
    });
    Record("VII. Reset", "LevelLoad rewind 5K/frame",
           cT2, mT2, kSubAllocs*kFrames, "marker save+rewind vs N x free()");
}

// =============================================================================
//  VIII. HANDLE RESOLUTION
//  Measures the seqlock-based handle table lookup in isolation.
//  Compared to a raw pointer dereference.
//  Quantifies the safety-indirection cost.
// =============================================================================

static void BenchHandleResolution()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t kHandles = 50'000;
    constexpr size_t kLookups = 2'000'000;

    std::vector<Handle> handles(kHandles);
    std::vector<void*>  rawPtrs(kHandles);
    for (size_t i = 0; i < kHandles; ++i) {
        handles[i] = eng.AllocateWithHandle<Bucket128>();
        rawPtrs[i] = malloc(128);
    }

    int64_t cT = MedianNs([&]{
        volatile size_t s = 0;
        for (size_t i = 0; i < kLookups; ++i) {
            Bucket128* p = eng.ResolveHandle<Bucket128>(handles[i % kHandles]);
            s += (uintptr_t)p & 1;
        }
        Sink((void*)&s);
    });
    int64_t mT = MedianNs([&]{
        volatile size_t s = 0;
        for (size_t i = 0; i < kLookups; ++i)
            s += (uintptr_t)rawPtrs[i % kHandles] & 1;
        Sink((void*)&s);
    });
    Record("VIII. Handles", "Handle resolve 2M ops",
           cT, mT, kLookups, "vs raw ptr deref");

    for (size_t i = 0; i < kHandles; ++i) { eng.FreeHandle<Bucket128>(handles[i]); free(rawPtrs[i]); }
}

// =============================================================================
//  IX. GAME SIMULATION
//  The most representative real-world benchmark:
//    - Per-frame scratch: many small short-lived allocs → Linear + Reset
//    - Entity pool: persistent objects recycled every 4 frames → Pool
//  Stresses both subsystems together under a realistic call pattern.
// =============================================================================

static void BenchGameSim()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t kFrames     = 2000;
    constexpr size_t kScratch    = 512;
    constexpr size_t kEntities   = 128;
    constexpr size_t kEntityLife = 4;

    std::vector<Handle> entities(kEntities, g_InvalidHandle);
    size_t frameIdx = 0;

    int64_t cT = MedianNs([&]{
        for (size_t f = 0; f < kFrames; ++f) {
            for (size_t i = 0; i < kScratch; ++i)
                Sink(eng.Allocate<FrameLoad>(32 + (i & 63)));
            if (frameIdx++ % kEntityLife == 0)
                for (size_t i = 0; i < kEntities; ++i) {
                    if (entities[i] != g_InvalidHandle) eng.FreeHandle<Bucket128>(entities[i]);
                    entities[i] = eng.AllocateWithHandle<Bucket128>();
                }
            eng.Reset<FrameLoad>();
        }
        for (auto hh : entities) if (hh != g_InvalidHandle) eng.FreeHandle<Bucket128>(hh);
        entities.assign(kEntities, g_InvalidHandle);
        frameIdx = 0;
    });

    std::vector<void*> mentities(kEntities, nullptr);
    size_t mframe = 0;
    int64_t mT = MedianNs([&]{
        for (size_t f = 0; f < kFrames; ++f) {
            std::vector<void*> scratch(kScratch);
            for (size_t i = 0; i < kScratch; ++i) scratch[i] = malloc(32 + (i & 63));
            if (mframe++ % kEntityLife == 0)
                for (size_t i = 0; i < kEntities; ++i) {
                    if (mentities[i]) free(mentities[i]);
                    mentities[i] = malloc(128);
                }
            for (auto p : scratch) free(p);
        }
        for (auto p : mentities) if (p) free(p);
        mentities.assign(kEntities, nullptr);
        mframe = 0;
    });

    const size_t ops = kFrames * kScratch + (kFrames / kEntityLife) * kEntities;
    Record("IX. Game Sim", "2K frame mixed loop",
           cT, mT, ops, "scratch+pool+reset");
}

// =============================================================================
//  X. FALSE-SHARING PROBE
//  16 threads do pure bump allocations on their own TLS slab simultaneously.
//  If ThreadLocalData is not padded to exactly 64 bytes, adjacent threads'
//  TLS structs share a cache line — every bump-ptr write invalidates all
//  neighbours, producing high inter-thread variance.
//
//  Prints a per-thread breakdown and cluster analysis independently of the
//  main table.  Included in table as avg-time comparison.
// =============================================================================

static void BenchFalseSharing()
{
    AllocatorEngine eng(kSlabSize, kArenaSize);
    eng.Initialize();


    constexpr size_t kThreads     = 16;
    // 16 threads x 40K allocs x 128B = 80MB < 128MB arena budget.
    constexpr size_t kAllocsPerTh = 40'000;

    // Pre-warm: each thread acquires its slab chain before the timed phase.
    {
        std::barrier wb(static_cast<std::ptrdiff_t>(kThreads));
        std::vector<std::thread> wv;
        wv.reserve(kThreads);
        for (size_t t = 0; t < kThreads; ++t)
            wv.emplace_back([&]{
                wb.arrive_and_wait();
                for (size_t i = 0; i < kAllocsPerTh; ++i) Sink(eng.Allocate<FrameLoad>(128));
                LinearStrategyModule<FrameLoad>::Reset();
            });
        for (auto& t : wv) t.join();
    }

    std::array<int64_t, kThreads> cUs{}, mUs{};

    // ── Timed: custom ─────────────────────────────────────────────────────
    {
        std::barrier go(static_cast<std::ptrdiff_t>(kThreads));
        std::vector<std::thread> tv; tv.reserve(kThreads);
        for (size_t t = 0; t < kThreads; ++t)
            tv.emplace_back([&, t]{
                go.arrive_and_wait();
                int64_t t0 = NowNs();
                for (size_t i = 0; i < kAllocsPerTh; ++i) Sink(eng.Allocate<FrameLoad>(128));
                cUs[t] = (NowNs() - t0) / 1000;
                LinearStrategyModule<FrameLoad>::Reset();
            });
        for (auto& t : tv) t.join();
    }

    // ── Timed: malloc ─────────────────────────────────────────────────────
    {
        std::barrier go(static_cast<std::ptrdiff_t>(kThreads));
        std::vector<std::thread> tv; tv.reserve(kThreads);
        for (size_t t = 0; t < kThreads; ++t)
            tv.emplace_back([&, t]{
                std::vector<void*> p(kAllocsPerTh);
                go.arrive_and_wait();
                int64_t t0 = NowNs();
                for (size_t i = 0; i < kAllocsPerTh; ++i) p[i] = malloc(128);
                mUs[t] = (NowNs() - t0) / 1000;
                for (auto x : p) free(x);
            });
        for (auto& t : tv) t.join();
    }

    int64_t cMin = *std::min_element(cUs.begin(), cUs.end());
    int64_t cMax = *std::max_element(cUs.begin(), cUs.end());
    int64_t cAvg = std::accumulate(cUs.begin(), cUs.end(), 0LL) / kThreads;
    int64_t mMin = *std::min_element(mUs.begin(), mUs.end());
    int64_t mMax = *std::max_element(mUs.begin(), mUs.end());
    int64_t mAvg = std::accumulate(mUs.begin(), mUs.end(), 0LL) / kThreads;
    float cVar = cAvg > 0 ? float(cMax - cMin) / float(cAvg) : 0.f;
    float mVar = mAvg > 0 ? float(mMax - mMin) / float(mAvg) : 0.f;

    if (!g_CSV) {
        // Cluster analysis: sort, find biggest gap.
        std::array<int64_t, kThreads> sorted = cUs;
        std::sort(sorted.begin(), sorted.end());
        int64_t bigGap = 0; size_t splitAt = 0;
        for (size_t i = 1; i < kThreads; ++i) {
            int64_t g = sorted[i] - sorted[i-1];
            if (g > bigGap) { bigGap = g; splitAt = i; }
        }
        bool twoClusters = (bigGap > cAvg / 6) && splitAt > 0 && splitAt < kThreads;

        // Within-cluster variance for each cluster.
        auto wvar = [&](size_t lo, size_t hi) -> float {
            if (lo >= hi) return 0.f;
            int64_t mid = sorted[(lo + hi) / 2];
            return mid > 0 ? float(sorted[hi-1] - sorted[lo]) / float(mid) : 0.f;
        };
        float vA = wvar(0, splitAt), vB = wvar(splitAt, kThreads);

        std::cout << "\n[X. False-Sharing Probe — 16 threads, 100K bump allocs each]\n";
        std::cout << "  Custom: avg=" << cAvg << "µs  min=" << cMin
                  << "µs  max=" << cMax << "µs  spread="
                  << std::fixed << std::setprecision(1) << (cVar*100.f) << "%\n";
        std::cout << "  malloc: avg=" << mAvg << "µs  min=" << mMin
                  << "µs  max=" << mMax << "µs  spread="
                  << std::fixed << std::setprecision(1) << (mVar*100.f) << "%\n";

        if (twoClusters && vA < 0.20f && vB < 0.20f)
            std::cout << "  >> PASS (HT-pair clusters at "
                      << sorted[0] << "-" << sorted[splitAt-1] << "µs and "
                      << sorted[splitAt] << "-" << sorted[kThreads-1]
                      << "µs). Gap=" << bigGap << "µs is scheduler, not false sharing.\n";
        else if (cVar < 0.30f)
            std::cout << "  >> PASS: single cluster, spread="
                      << (cVar*100.f) << "%. TLS padding correct.\n";
        else
            std::cout << "  >> WARN: spread=" << (cVar*100.f)
                      << "% with no clear cluster. Possible false sharing.\n"
                      << "  >> Check sizeof(ThreadLocalData) % 64 == 0 in linear_module.h.\n";
    }

    char notes[64];
    snprintf(notes, sizeof(notes), "custom spread=%.0f%%  malloc spread=%.0f%%",
             cVar*100.f, mVar*100.f);
    Record("X. False-Sharing", "TLS isolation 16T",
           cAvg*1000, mAvg*1000, kAllocsPerTh, notes);
}

// =============================================================================
//  MAIN
// =============================================================================

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--csv") g_CSV = true;

    if (!g_CSV) {
        printf("\n╔══════════════════════════════════════════════════════════════╗\n");
        printf("║  CUSTOM ALLOCATOR vs malloc  —  Full Benchmark Suite         ║\n");
        printf("║  Arena: 128MB | Slab: 64KB | Median of %d runs per bench      ║\n", kRuns);
        printf("╚══════════════════════════════════════════════════════════════╝\n\n");
    }

    const struct { const char* label; void(*fn)(); } suite[] = {
        { "  I.    Throughput",         BenchThroughput      },
        { "  II.   Churn",              BenchChurn           },
        { "  III.  Size sweep",         BenchSizeSweep       },
        { "  IV.   Cache coherency",    BenchCacheCoherency  },
        { "  V.    Fragmentation",      BenchFragmentation   },
        { "  VI.   Concurrency",        BenchConcurrency     },
        { "  VII.  Reset / Rewind",     BenchReset           },
        { "  VIII. Handle resolution",  BenchHandleResolution},
        { "  IX.   Game simulation",    BenchGameSim         },
        { "  X.    False-sharing probe",BenchFalseSharing    },
    };

    for (const auto& s : suite) {
        if (!g_CSV) printf("%s...\n", s.label);
        s.fn();
    }

    if (g_CSV) PrintCSV();
    else       PrintTable();

    return 0;
}
