#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "coop/work/erg.h"
#include "coop/work/detail/deque.h"

// ---------------------------------------------------------------------------
// Gating microbenchmarks for the two speculative concurrency builds in the
// backlog: a per-cooperator Erg slab with cross-thread free (#8) and a
// block-based / owner-cooperative deque to enable chunked steals (#4).
//
// Both builds are expensive and both are justified ONLY if the substrate unit
// they replace is hot. bench_fanout.cpp measures the whole pipeline and cannot
// price a single new/delete or a single steal CAS. These benchmarks isolate
// exactly those two units so the go/no-go is a number, not a hunch:
//
//   - Erg alloc/free  -> greenlights or kills #8 (the slab must beat plain
//                        new/delete, and especially malloc's CROSS-THREAD free,
//                        which is what a stealer on another core pays).
//   - one steal CAS   -> greenlights or kills #4 (chunking only pays if the
//                        per-item steal cost is a meaningful fraction of a
//                        morsel's runtime; the balancing work puts irregular
//                        morsels at tens of microseconds).
//
// All numbers are algorithm-intrinsic where possible (success/attempt counts on
// the steal path) so a shared, noisy box does not corrupt the verdict.
// ---------------------------------------------------------------------------

namespace
{

// A no-op Erg body: the smallest possible Run(), so the benchmark prices the
// allocation, the virtual dispatch, and the free -- not the work.
//
struct Noop
{
    void operator()() const { benchmark::DoNotOptimize(this); }
};

} // namespace

// -- (1a) Erg alloc + run + free, all on one thread -------------------------
//
// Steady-state cost of a shed-then-locally-stolen Erg: the common Grid path
// where the cooperator that shed the work is also the one that runs and frees
// it. This is the floor #8 must beat for the no-migration case.
//
static void BM_ErgAllocFree(benchmark::State& state)
{
    for (auto _ : state)
    {
        coop::work::Erg* e = coop::work::MakeErg(Noop{});
        coop::work::RunErg(e);              // runs Noop, then delete
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ErgAllocFree);

// -- (1b) Run only, no allocation -------------------------------------------
//
// The same Erg dispatched from the stack, no new/delete. Subtracting this from
// (1a) isolates the pure malloc+free cost from the virtual call overhead.
//
static void BM_ErgRunOnly(benchmark::State& state)
{
    coop::work::ErgImpl<Noop> e{Noop{}};
    coop::work::Erg* p = &e;
    for (auto _ : state)
    {
        p->Run();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ErgRunOnly);

// -- (1c) Cross-thread free -------------------------------------------------
//
// The case a slab actually has to beat: an Erg shed (allocated) on one core and
// run+freed by a stealer on another. glibc malloc returns the block to the
// allocating arena under a lock, so the free crosses cores. A pinned consumer
// thread drains a bounded SPSC ring and frees; the benchmark thread allocates
// and pushes. Backpressure makes the reported ns/Erg the throughput of the
// slower side of the alloc||cross-free pipeline -- the real steady-state cost.
//
static void BM_ErgAllocFree_CrossThread(benchmark::State& state)
{
    constexpr size_t kRing = 4096;             // power of two
    constexpr size_t kMask = kRing - 1;
    std::vector<std::atomic<coop::work::Erg*>> ring(kRing);
    for (auto& slot : ring)
    {
        slot.store(nullptr, std::memory_order_relaxed);
    }

    std::atomic<bool> stop{false};
    alignas(64) std::atomic<uint64_t> head{0};  // consumer reads here
    alignas(64) std::atomic<uint64_t> tail{0};  // producer writes here

    std::thread consumer([&]
    {
        // Pin the consumer to a sibling core so the free genuinely crosses cores.
        //
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(3, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);

        uint64_t h = 0;
        while (true)
        {
            coop::work::Erg* e = ring[h & kMask].exchange(nullptr, std::memory_order_acquire);
            if (e == nullptr)
            {
                if (stop.load(std::memory_order_acquire) &&
                    head.load(std::memory_order_relaxed) == tail.load(std::memory_order_acquire))
                {
                    break;
                }
                continue;
            }
            coop::work::RunErg(e);              // Run + cross-thread delete
            ++h;
            head.store(h, std::memory_order_release);
        }
    });

    uint64_t t = 0;
    for (auto _ : state)
    {
        coop::work::Erg* e = coop::work::MakeErg(Noop{});
        // Wait for a free ring slot (consumer has drained it).
        //
        while (t - head.load(std::memory_order_acquire) >= kRing)
        {
            benchmark::DoNotOptimize(t);
        }
        ring[t & kMask].store(e, std::memory_order_release);
        ++t;
        tail.store(t, std::memory_order_release);
    }

    stop.store(true, std::memory_order_release);
    consumer.join();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ErgAllocFree_CrossThread)->UseRealTime();

// -- (1d) Cross-thread handoff only, no malloc ------------------------------
//
// Same SPSC pipeline as (1c) but the payload is a single pre-allocated Erg that
// is handed across and never freed -- so this prices only the ring handoff and
// cache-line bounce, with zero malloc traffic. (1c) minus (1d) is the part of
// the cross-thread cost that is genuinely malloc/free arena contention, i.e.
// the part a slab with a cross-thread free-list could actually remove.
//
static void BM_CrossThreadHandoffOnly(benchmark::State& state)
{
    constexpr size_t kRing = 4096;
    constexpr size_t kMask = kRing - 1;
    std::vector<std::atomic<coop::work::Erg*>> ring(kRing);
    for (auto& slot : ring)
    {
        slot.store(nullptr, std::memory_order_relaxed);
    }

    coop::work::Erg* token = coop::work::MakeErg(Noop{});   // one block, reused forever
    std::atomic<bool> stop{false};
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};

    std::thread consumer([&]
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(3, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        uint64_t h = 0;
        while (true)
        {
            coop::work::Erg* e = ring[h & kMask].exchange(nullptr, std::memory_order_acquire);
            if (e == nullptr)
            {
                if (stop.load(std::memory_order_acquire) &&
                    head.load(std::memory_order_relaxed) == tail.load(std::memory_order_acquire))
                {
                    break;
                }
                continue;
            }
            benchmark::DoNotOptimize(e);       // touch it like a real consumer would
            ++h;
            head.store(h, std::memory_order_release);
        }
    });

    uint64_t t = 0;
    for (auto _ : state)
    {
        while (t - head.load(std::memory_order_acquire) >= kRing)
        {
            benchmark::DoNotOptimize(t);
        }
        ring[t & kMask].store(token, std::memory_order_release);
        ++t;
        tail.store(t, std::memory_order_release);
    }

    stop.store(true, std::memory_order_release);
    consumer.join();
    delete token;
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CrossThreadHandoffOnly)->UseRealTime();

// -- (2a) Uncontended steal cost --------------------------------------------
//
// One thread alone: push an item, steal it back, repeat. No concurrent thief
// and no owner/thief scheduling, so this is the scheduling-noise-free floor for
// a single Steal -- the seq_cst fence plus the lock cmpxchg on m_top. The push
// is priced separately (next benchmark) and subtracted. This number survives a
// saturated box; the contended sweep below does not, so this is the steal CAS
// unit cost #4 has to amortize.
//
static void BM_StealUncontended(benchmark::State& state)
{
    coop::work::detail::Deque<void*, 8192> deque;
    void* item = reinterpret_cast<void*>(uintptr_t{0x1});
    void* out;
    for (auto _ : state)
    {
        deque.PushBottom(item);            // refill the one slot the steal will take
        benchmark::DoNotOptimize(deque.Steal(out));       // Steal: seq_cst fence + lock cmpxchg
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StealUncontended);

// Owner-local push+pop round trip, for scale: PushBottom + a non-last PopBottom share the same
// seq_cst fence Steal pays but pop takes the LIFO bottom with no cross-thread CAS in the common
// (non-last-element) case. The gap to BM_StealUncontended is the locked top CAS Steal adds.
//
static void BM_OwnerPushPop(benchmark::State& state)
{
    coop::work::detail::Deque<void*, 8192> deque;
    void* item = reinterpret_cast<void*>(uintptr_t{0x1});
    void* out;
    deque.PushBottom(item);                // keep one resident so pops are never last-element
    for (auto _ : state)
    {
        deque.PushBottom(item);
        deque.PopBottom(out);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OwnerPushPop);

// -- (2) Steal unit cost vs thief count -------------------------------------
//
// One owner thread keeps a Chase-Lev deque non-empty (PushBottom in a tight
// loop); N pinned thieves Steal concurrently. Because the owner outpaces the
// thieves, a failed Steal is almost always a lost CAS race on m_top -- not an
// empty deque -- so the success/attempt ratio measures the m_top cache-line
// bounce directly. Reported per thief-count sweep point:
//
//   contention    failed attempts per success (CAS races lost = line bounce).
//   success_rate  fraction of Steal() calls that won their m_top CAS.
//
// The contention / success_rate ratios are algorithm-intrinsic and survive a
// loaded box. The items/s (and any ns/steal derived from it) is wall-clock and
// is only trustworthy when every thief + the owner each own an idle physical
// core; under oversubscription it reports the scheduling tax, not the steal.
// Read the ratios for the contention shape; read BM_StealUncontended for the
// unit cost. Manual timing: the loop spins a fixed real-time window and counts
// the steal delta over it, so thread create/join never taxes the interval.
//
static void BM_StealUnitCost(benchmark::State& state)
{
    const int nThieves = static_cast<int>(state.range(0));
    coop::work::detail::Deque<void*, 8192> deque;

    std::atomic<bool> stop{false};
    alignas(64) std::atomic<uint64_t> successes{0};
    alignas(64) std::atomic<uint64_t> attempts{0};

    // Per-thief live counters, one cache line each, so the main thread can read a running steal
    // total mid-run without forcing thieves to share an atomic (which would itself add the very
    // m_top-style contention we are measuring). 8 uint64 == 64 bytes of stride.
    //
    std::vector<std::atomic<uint64_t>> liveSucc(static_cast<size_t>(nThieves) * 8);
    for (auto& c : liveSucc)
    {
        c.store(0, std::memory_order_relaxed);
    }

    // Owner: keep the deque full so thieves always race on a real item.
    //
    std::thread owner([&]
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(2, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        void* item = reinterpret_cast<void*>(uintptr_t{0x1});
        while (!stop.load(std::memory_order_relaxed))
        {
            deque.PushBottom(item);            // false when full; just spin-retry
        }
    });

    std::vector<std::thread> thieves;
    for (int i = 0; i < nThieves; ++i)
    {
        thieves.emplace_back([&, i]
        {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(4 + i, &set);              // thieves on cores 4..4+N-1
            pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
            uint64_t s = 0, a = 0;
            std::atomic<uint64_t>& mine = liveSucc[static_cast<size_t>(i) * 8];
            void* out;
            while (!stop.load(std::memory_order_relaxed))
            {
                ++a;
                if (deque.Steal(out))
                {
                    ++s;
                    if ((s & 0x3FF) == 0)
                    {
                        mine.store(s, std::memory_order_relaxed);   // publish ~every 1024 steals
                    }
                }
            }
            mine.store(s, std::memory_order_relaxed);
            successes.fetch_add(s, std::memory_order_relaxed);
            attempts.fetch_add(a, std::memory_order_relaxed);
        });
    }

    // Let the threads reach steady state before measuring.
    //
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto liveTotal = [&]
    {
        uint64_t sum = 0;
        for (int i = 0; i < nThieves; ++i)
        {
            sum += liveSucc[static_cast<size_t>(i) * 8].load(std::memory_order_relaxed);
        }
        return sum;
    };

    uint64_t totalSteals = 0;
    for (auto _ : state)
    {
        uint64_t s0 = liveTotal();
        auto t0 = std::chrono::steady_clock::now();
        // Spin a fixed real-time window; count steals over it.
        //
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2))
        {
            benchmark::DoNotOptimize(s0);
        }
        auto t1 = std::chrono::steady_clock::now();
        uint64_t s1 = liveTotal();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        uint64_t delta = s1 - s0;
        totalSteals += delta;
        // Report the real window as the iteration time and the steals over it as the items;
        // gbench then computes items/s = aggregate steal throughput, ns/steal = 1e9 / that.
        //
        state.SetIterationTime(secs);
    }

    stop.store(true, std::memory_order_relaxed);
    owner.join();
    for (auto& th : thieves)
    {
        th.join();
    }

    uint64_t s = successes.load(), a = attempts.load();
    state.SetItemsProcessed(totalSteals);
    state.counters["thieves"] = nThieves;
    state.counters["contention"] =
        s > 0 ? static_cast<double>(a - s) / static_cast<double>(s) : 0.0;
    state.counters["success_rate"] =
        a > 0 ? static_cast<double>(s) / static_cast<double>(a) : 0.0;
}
BENCHMARK(BM_StealUnitCost)->Arg(1)->Arg(2)->Arg(3)->Arg(4)->UseManualTime();
