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

static void RunBenchmarkCfg(benchmark::State& state,
    coop::CooperatorConfiguration const& config,
    std::function<void(coop::Context*, benchmark::State&)> fn)
{
    coop::Cooperator cooperator(config);
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

static void RunBenchmark(benchmark::State& state,
    std::function<void(coop::Context*, benchmark::State&)> fn)
{
    RunBenchmarkCfg(state, coop::s_defaultCooperatorConfiguration, std::move(fn));
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
// BM_Scheduler_Yield_Direct — direct context-to-context yield, off vs on
// ---------------------------------------------------------------------------
//
// Prices the directYield fastpath (CooperatorConfiguration::directYield) against the default
// trampoline-through-the-loop yield, at matched context counts. The fastpath only engages when
// another context is runnable, so this is meaningful at N >= 2.
//
// arg0 selects directYield off(0)/on(1); arg1 is the round-robin context count. The off rows
// reproduce the default scaled yield as a baseline; the on rows show the fastpath win. Because the
// budget forces a poll fallback through the cooperator loop every directYieldBudget switches, the
// on rows also fold in the amortized cost of that bound — so this is the honest steady-state
// number, not a poll-free best case. This is the artifact for the "prove across regimes before it
// can ship default" bar; directYield ships off by default.
//
static void BM_Scheduler_Yield_Direct(benchmark::State& state)
{
    const bool direct = state.range(0) != 0;
    const int  n      = static_cast<int>(state.range(1));

    coop::CooperatorConfiguration cfg = coop::s_defaultCooperatorConfiguration;
    cfg.directYield = direct;

    RunBenchmarkCfg(state, cfg, [n](coop::Context* ctx, benchmark::State& state)
    {
        auto* co = ctx->GetCooperator();

        // N-1 background yielders are the round-robin partners the fastpath switches into.
        //
        for (int i = 0; i < n - 1; i++)
        {
            co->Spawn([](coop::Context* c)
            {
                while (!c->IsKilled()) c->Yield(true);
            });
        }

        // Let them all reach their first yield before timing.
        //
        ctx->Yield(true);

        for (auto _ : state)
        {
            ctx->Yield(true);
        }

        // The scheduler cycles all N contexts between two yields of the measured one, so each
        // iteration is ~N context switches. Reporting N items/iteration makes the throughput
        // column read as ~switches/s, directly comparable to the bare per-switch cost.
        //
        state.SetItemsProcessed(state.iterations() * n);
    });
}
BENCHMARK(BM_Scheduler_Yield_Direct)
    ->Args({0, 2})->Args({1, 2})
    ->Args({0, 8})->Args({1, 8})
    ->Args({0, 64})->Args({1, 64});

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
