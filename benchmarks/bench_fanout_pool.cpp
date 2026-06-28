#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "coop/context.h"
#include "coop/continuation.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/coordinator.h"
#include "coop/io/handle.h"
#include "coop/io/timeout.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"
#include "coop/work/grid.h"

// ---------------------------------------------------------------------------
// Clustered-imbalance makespan: the headline workload the cross-thread work
// substrate exists for, and the instrument its makespan covenant is closed
// against. The question is narrow and load-balancing-specific: when irregular
// compute lands lopsidedly on one core, can a work-stealing pool spread it and
// beat a static shared-nothing layout that cannot move work once it is placed?
//
// Workload. M cooperators, each pinned to its own physical core. N independent
// IO pipelines; each pipeline runs `kStages` stages, and a stage is irregular
// `BusyFor` compute followed by an io_uring timed sleep (the IO wait). The
// per-pipeline compute cost is drawn once from a wide distribution, so morsels
// are irregular -- the regime where static balancing is hard. The imbalance is
// *clustered* by construction: every pipeline originates on shard 0, so 100% of
// the compute is dumped on one core's worth of static placement.
//
//   BM_FanOut_Pool_StaticShard -- baseline. Each pipeline is a context bound to
//       its origin cooperator and never moves (the "static shard" / shared-
//       nothing strategy). Clustered, that means shard 0's core chews the whole
//       compute alone while the others idle; makespan is bound by the heaviest
//       shard.
//   BM_FanOut_Pool_Grid -- the substrate. Each pipeline is a reusable Erg shed
//       into a work::Grid and re-shed from its timer's completion continuation.
//       The cluster on shard 0 is stolen and spread across all M cores;
//       makespan approaches total_compute / M, floored by the per-pipeline
//       sleep critical path.
//
// Same pipelines, same per-stage compute and sleep, same clustered origin --
// only the movement policy differs. The reported Time is the end-to-end
// makespan (manual-timed, so per-iteration cooperator/grid construction and
// teardown are excluded). A balanced variant (arg 1 == 1) spreads the origin
// across all cooperators; it is the robustness check that stealing is an opt-in
// relief valve for imbalance, not a tax on the already-balanced case.
// ---------------------------------------------------------------------------

namespace
{

using Clock = std::chrono::steady_clock;

inline uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

// Spin for ~ns of wall time. A pipeline stage's compute; deliberately not a
// sleep, so it occupies the core the way real per-morsel work would.
//
inline void BusyFor(uint64_t ns)
{
    if (!ns)
    {
        return;
    }
    const uint64_t start = NowNs();
    volatile uint64_t x = 0;
    while (NowNs() - start < ns)
    {
        for (int i = 0; i < 64; i++)
        {
            x = x + static_cast<uint64_t>(i);
        }
    }
    (void)x;
}

// Fixed workload knobs. N stays well under the per-shard deque capacity so the
// clustered initial shed never overflows shard 0.
//
constexpr int      kPipelines    = 400;
constexpr int      kStages       = 5;
constexpr int64_t  kSleepUs      = 2000;    // inter-stage IO wait
constexpr uint64_t kComputeLoNs  = 40000;   // irregular per-pipeline compute, low
constexpr uint64_t kComputeHiNs  = 120000;  // ... and high (mean ~80us morsels)
constexpr unsigned kSeed = 0x5eedfa11u;

// Per-pipeline compute weights, drawn once with a fixed seed so both strategies
// see the identical irregular workload across runs.
//
std::vector<uint64_t> ComputeWeights()
{
    std::mt19937 rng(kSeed);
    std::uniform_int_distribution<uint64_t> dist(kComputeLoNs, kComputeHiNs);
    std::vector<uint64_t> w(kPipelines);
    for (int i = 0; i < kPipelines; i++)
    {
        w[i] = dist(rng);
    }
    return w;
}

struct Driver
{
    coop::time::Interval sleep;
    std::atomic<int>     remaining;
    std::atomic<int>     ranOn[8] = {};
};

// A reusable pipeline Erg: caller-owned (never freed by the stealer) so the same
// object re-sheds itself each stage with no per-stage allocation of the work
// item. Each Run() is one stage; the stealer that pulls it runs the compute,
// arms the IO sleep, and a detached continuation re-sheds this Erg when the timer
// fires -- landing on the running cooperator's own shard, stealable from there.
//
struct GridPipeline final : coop::work::Erg
{
    Driver*  drv      = nullptr;
    uint64_t computeNs = 0;
    int      remaining = 0;

    GridPipeline() { m_stealerOwned = false; }

    void Run() final
    {
        if (coop::work::Participation* p = coop::GetCooperator()->m_participation)
        {
            drv->ranOn[p->shard].fetch_add(1, std::memory_order_relaxed);
        }
        BusyFor(computeNs);
        if (--remaining <= 0)
        {
            drv->remaining.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }

        // Arm the inter-stage IO sleep and hand the follow-on to a detached
        // continuation. The Erg's Run() must return promptly (run-to-completion
        // on the shared stealer), so the next stage cannot block here -- it
        // rides the timer's completion. Heap-held so it outlives this Run().
        //
        auto* w = new StageWait(coop::Self());
        coop::io::Timeout(w->handle, drv->sleep);
        w->coord.ContinueDetached([this, w](coop::Coordinator*)
        {
            // Fires on the cooperator that submitted the timer; the prior Run()
            // has long since retired, honoring the reusable-Erg single-owner
            // contract. Re-shed to this cooperator's local shard.
            //
            delete w;
            coop::Shed(static_cast<coop::work::Erg*>(this));
        });
    }

    // One in-flight timer per stage, reused per stage; heap-resident so it
    // survives the Run() that armed it and is freed by the completion.
    //
    struct StageWait
    {
        coop::Coordinator coord;
        coop::io::Handle  handle;
        explicit StageWait(coop::Context* ctx) : handle(ctx, coop::GetUring(), &coord) {}
    };
};

// Core the i-th cooperator pins to. The makespan demands one dedicated physical
// core per cooperator, so by default cooperator i pins to cpu i. Two overrides:
// POOL_PINCORES is a comma-separated cpu list (pin to a chosen set, e.g. to
// dodge cores other tenants hold on a shared host); POOL_PINCORES=none returns
// -1, leaving the threads unpinned so the OS scheduler places them on whatever
// cores are free -- the right choice on a contended box, where a hard pin onto a
// busy core starves a cooperator and serializes the makespan behind it.
//
int PinCore(int i)
{
    static const std::vector<int> cores = []
    {
        std::vector<int> v;
        const char* s = getenv("POOL_PINCORES");
        if (s && std::string(s) == "none")
        {
            v.push_back(-1);
            return v;
        }
        if (s)
        {
            for (const char* p = s; *p;)
            {
                v.push_back(atoi(p));
                while (*p && *p != ',') p++;
                while (*p == ',') p++;
            }
        }
        return v;
    }();
    return cores.empty() ? i : cores[i % cores.size()];
}

// Build M cooperators, each on its own thread pinned to a distinct physical
// core (this host pairs hyperthread siblings, so cpus 0..M-1 are one-per-core).
//
struct Fabric
{
    explicit Fabric(int m) : coops(m), threads(m)
    {
        for (int i = 0; i < m; i++)
        {
            coops[i] = new coop::Cooperator();
            threads[i] = new coop::Thread(coops[i]);
            const int core = PinCore(i);
            if (core >= 0)
            {
                threads[i]->PinToCore(core);
            }
        }
    }

    ~Fabric()
    {
        for (auto* c : coops)
        {
            c->Shutdown();
        }
        for (size_t i = 0; i < coops.size(); i++)
        {
            delete threads[i];
            delete coops[i];
        }
    }

    std::vector<coop::Cooperator*> coops;
    std::vector<coop::Thread*>     threads;
};

// Block the benchmark thread until every pipeline has finished all its stages.
// Polls; the poll granularity (100us) is negligible against a multi-ms makespan.
//
void AwaitDrain(Driver& drv)
{
    while (drv.remaining.load(std::memory_order_acquire) > 0)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

} // namespace

// -- Grid: clustered load spread by work-stealing -----------------------------
//
static void BM_FanOut_Pool_Grid(benchmark::State& state)
{
    const int  M         = static_cast<int>(state.range(0));
    const bool balanced  = state.range(1) != 0;
    const std::vector<uint64_t> weights = ComputeWeights();

    for (auto _ : state)
    {
        Fabric fab(M);
        coop::work::Grid grid;
        grid.Init(M);
        for (int m = 0; m < M; m++)
        {
            grid.Join(fab.coops[m]);
        }

        std::vector<GridPipeline> pipes(kPipelines);
        Driver drv;
        drv.sleep = std::chrono::microseconds(kSleepUs);
        drv.remaining.store(kPipelines, std::memory_order_relaxed);
        for (int i = 0; i < kPipelines; i++)
        {
            pipes[i].drv = &drv;
            pipes[i].computeNs = weights[i];
            pipes[i].remaining = kStages;
        }

        // Let every cooperator publish its participation field and spawn its
        // stealer before we start the clock.
        //
        std::this_thread::sleep_for(std::chrono::milliseconds(15));

        const auto t0 = Clock::now();

        // Shed each pipeline onto its origin shard (Shed pushes to the local
        // shard, so the shed must run on that cooperator's thread). Clustered:
        // all on cooperator 0. Balanced: round-robin origins.
        //
        for (int m = 0; m < M; m++)
        {
            const int origin = balanced ? m : 0;
            fab.coops[origin]->Submit([&pipes, M, origin, balanced](coop::Context*)
            {
                for (int i = 0; i < kPipelines; i++)
                {
                    if (!balanced || (i % M) == origin)
                    {
                        coop::Shed(static_cast<coop::work::Erg*>(&pipes[i]));
                    }
                }
            });
            if (!balanced)
            {
                break;
            }
        }

        AwaitDrain(drv);
        const auto t1 = Clock::now();
        state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());

        if (getenv("POOL_DEBUG"))
        {
            fprintf(stderr, "[grid M=%d bal=%d] %.1fms pulls=%lu parks=%lu maxidle=%lu ranOn=",
                M, balanced ? 1 : 0, std::chrono::duration<double, std::milli>(t1 - t0).count(),
                (unsigned long)grid.Pulls(), (unsigned long)grid.Parks(),
                (unsigned long)grid.MaxIdleRun());
            for (int m = 0; m < M; m++)
                fprintf(stderr, "%d ", drv.ranOn[m].load(std::memory_order_relaxed));
            fprintf(stderr, "\n");
        }
    }

    state.SetItemsProcessed(state.iterations() * int64_t{kPipelines} * kStages);
    state.counters["M"] = M;
    state.counters["pipes"] = kPipelines;
    state.counters["stages"] = kStages;
}
BENCHMARK(BM_FanOut_Pool_Grid)
    ->Args({3, 0})
    ->Args({3, 1})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// -- Static shared-nothing: clustered load cannot move ------------------------
//
static void BM_FanOut_Pool_StaticShard(benchmark::State& state)
{
    const int  M        = static_cast<int>(state.range(0));
    const bool balanced = state.range(1) != 0;
    const std::vector<uint64_t> weights = ComputeWeights();

    for (auto _ : state)
    {
        Fabric fab(M);

        Driver drv;
        drv.sleep = std::chrono::microseconds(kSleepUs);
        drv.remaining.store(kPipelines, std::memory_order_relaxed);

        // No settle needed: there is no grid and no stealer to spin up.
        //
        const auto t0 = Clock::now();

        // Each pipeline runs as a context bound to its origin cooperator and
        // never migrates. Detach so the transient kick context exiting does not
        // kill them. Clustered: every context on cooperator 0, the others idle.
        //
        for (int m = 0; m < M; m++)
        {
            const int origin = balanced ? m : 0;
            const coop::time::Interval sleep = drv.sleep;
            fab.coops[origin]->Submit(
                [&weights, &drv, sleep, M, origin, balanced](coop::Context*)
            {
                for (int i = 0; i < kPipelines; i++)
                {
                    if (balanced && (i % M) != origin)
                    {
                        continue;
                    }
                    const uint64_t computeNs = weights[i];
                    coop::Spawn([&drv, sleep, computeNs](coop::Context* ctx)
                    {
                        ctx->Detach();
                        for (int s = 0; s < kStages; s++)
                        {
                            BusyFor(computeNs);
                            if (coop::time::Sleep(ctx, sleep) != coop::time::SleepResult::Ok)
                            {
                                return;     // killed (shutdown); stop early
                            }
                        }
                        drv.remaining.fetch_sub(1, std::memory_order_acq_rel);
                    });
                }
            });
            if (!balanced)
            {
                break;
            }
        }

        AwaitDrain(drv);
        const auto t1 = Clock::now();
        state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
    }

    state.SetItemsProcessed(state.iterations() * int64_t{kPipelines} * kStages);
    state.counters["M"] = M;
    state.counters["pipes"] = kPipelines;
    state.counters["stages"] = kStages;
}
BENCHMARK(BM_FanOut_Pool_StaticShard)
    ->Args({3, 0})
    ->Args({3, 1})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
