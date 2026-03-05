#include <functional>

#include <benchmark/benchmark.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "coop/perf/counters.h"
#include "coop/perf/patch.h"

// ---------------------------------------------------------------------------
// Helper: run a benchmark body inside a cooperator
// ---------------------------------------------------------------------------

struct BenchmarkArgs
{
    benchmark::State* state;
    std::function<void(coop::Context*, benchmark::State&)>* fn;
};

static void RunBenchmark(benchmark::State& state,
    std::function<void(coop::Context*, benchmark::State&)> fn)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    BenchmarkArgs args;
    args.state = &state;
    args.fn = &fn;

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        auto* a = static_cast<BenchmarkArgs*>(arg);
        (*a->fn)(ctx, *a->state);
        ctx->GetCooperator()->Shutdown();
    }, &args);
}

// ---------------------------------------------------------------------------
// BM_Perf_Yield — yield cost with current perf mode
// ---------------------------------------------------------------------------
//
// The yield round-trip is the scheduler hot path and hits the most probe sites per iteration:
// ContextYield, SchedulerLoop, ContextResume, PollCycle (and possibly PollSubmit). Comparing
// this across COOP_PERF_MODE=0 vs =2 isolates the per-probe overhead.
//
// For mode 2, run with probes disabled (default) to measure the JMP-skip cost, which is the
// interesting number — it's the tax paid when probes are compiled in but not active.
//
static void BM_Perf_Yield(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        for (auto _ : state)
        {
            ctx->Yield(true);
        }
    });
}
BENCHMARK(BM_Perf_Yield);

// ---------------------------------------------------------------------------
// BM_Perf_Yield_Enabled — yield cost with mode 2 probes enabled
// ---------------------------------------------------------------------------
//
// Only meaningful when compiled with COOP_PERF_MODE=2. When compiled with mode 0, this is
// identical to BM_Perf_Yield (Enable/Disable are no-ops).
//
static void BM_Perf_Yield_Enabled(benchmark::State& state)
{
    coop::perf::Enable();

    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        for (auto _ : state)
        {
            ctx->Yield(true);
        }
    });

    coop::perf::Disable();
}
BENCHMARK(BM_Perf_Yield_Enabled);

// ---------------------------------------------------------------------------
// BM_Perf_SpawnExit — spawn + exit lifecycle with current perf mode
// ---------------------------------------------------------------------------
//
// Each iteration spawns a context that immediately exits. Hits ContextSpawn + ContextExit
// probes plus the scheduler overhead. Captures allocation/deallocation cost alongside probe
// overhead.
//
static void BM_Perf_SpawnExit(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        auto* co = ctx->GetCooperator();

        for (auto _ : state)
        {
            coop::Coordinator coord(ctx);
            co->Spawn([&](coop::Context* child)
            {
                coord.Release(child, false);
            });
            coord.Acquire(ctx);
        }
    });
}
BENCHMARK(BM_Perf_SpawnExit);

// ---------------------------------------------------------------------------
// BM_Perf_SpawnExit_Enabled — spawn + exit with mode 2 probes enabled
// ---------------------------------------------------------------------------

static void BM_Perf_SpawnExit_Enabled(benchmark::State& state)
{
    coop::perf::Enable();

    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        auto* co = ctx->GetCooperator();

        for (auto _ : state)
        {
            coop::Coordinator coord(ctx);
            co->Spawn([&](coop::Context* child)
            {
                coord.Release(child, false);
            });
            coord.Acquire(ctx);
        }
    });

    coop::perf::Disable();
}
BENCHMARK(BM_Perf_SpawnExit_Enabled);

// ---------------------------------------------------------------------------
// BM_Perf_YieldScaled — yield with multiple contexts
// ---------------------------------------------------------------------------
//
// 16 contexts all yielding. The scheduler loop processes all of them between each measured
// iteration, so probe site overhead is amplified by context count.
//
static void BM_Perf_YieldScaled(benchmark::State& state)
{
    int n = state.range(0);

    RunBenchmark(state, [n](coop::Context* ctx, benchmark::State& state)
    {
        auto* co = ctx->GetCooperator();

        for (int i = 0; i < n - 1; i++)
        {
            co->Spawn([](coop::Context* c)
            {
                while (!c->IsKilled()) c->Yield(true);
            });
        }

        ctx->Yield(true);

        for (auto _ : state)
        {
            ctx->Yield(true);
        }
    });
}
BENCHMARK(BM_Perf_YieldScaled)->Arg(4)->Arg(16)->Arg(64);
