#include <functional>

#include <benchmark/benchmark.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/thread.h"

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
// BM_Scheduler_Yield — single context yielding in a tight loop
// ---------------------------------------------------------------------------
//
// Measures the minimum cost of one yield round-trip: Yield() -> longjmp to cooperator -> pop next
// from yielded list -> Resume() -> longjmp back. This is the baseline per-context-switch cost
// including the ticker and uring contexts getting their turns.
//
static void BM_Scheduler_Yield(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        for (auto _ : state)
        {
            ctx->Yield(true);
        }
    });
}
BENCHMARK(BM_Scheduler_Yield);

// ---------------------------------------------------------------------------
// BM_Scheduler_Yield_Scaled — yield throughput vs context count
// ---------------------------------------------------------------------------
//
// Spawns N-1 additional contexts each doing Yield(true) forever, then runs the benchmark loop on
// the original context. Measures how yield throughput degrades as the scheduler has more contexts
// to round-robin through. Each benchmark iteration is one yield of the measured context, but the
// scheduler processes all N contexts between iterations.
//
static void BM_Scheduler_Yield_Scaled(benchmark::State& state)
{
    int n = state.range(0);

    RunBenchmark(state, [n](coop::Context* ctx, benchmark::State& state)
    {
        auto* co = ctx->GetCooperator();

        // Spawn N-1 background yielders
        //
        for (int i = 0; i < n - 1; i++)
        {
            co->Spawn([](coop::Context* c)
            {
                while (!c->IsKilled()) c->Yield(true);
            });
        }

        // Let them all start
        //
        ctx->Yield(true);

        for (auto _ : state)
        {
            ctx->Yield(true);
        }
    });
}
BENCHMARK(BM_Scheduler_Yield_Scaled)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32)->Arg(64);

// ---------------------------------------------------------------------------
// BM_Scheduler_SpawnYieldExit — full context lifecycle
// ---------------------------------------------------------------------------
//
// Measures the full lifecycle cost: spawn a context, it yields once, then exits. Captures
// allocation + setup + one context switch + teardown per iteration.
//
static void BM_Scheduler_SpawnYieldExit(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        auto* co = ctx->GetCooperator();

        for (auto _ : state)
        {
            coop::Coordinator coord(ctx);
            co->Spawn([&](coop::Context* child)
            {
                child->Yield(true);
                coord.Release(child, false);
            });
            coord.Acquire(ctx);
        }
    });
}
BENCHMARK(BM_Scheduler_SpawnYieldExit);
