#include <cstdint>
#include <functional>

#include <benchmark/benchmark.h>

#include "coop/context.h"
#include "coop/continuation.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/thread.h"

// ---------------------------------------------------------------------------
// Helper: run a benchmark body inside a cooperator (mirrors bench_coordinator)
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
// Continuation dispatch cost
// ---------------------------------------------------------------------------

// Register a continuation on a held coordinator and fire it. The continuation Resumes as a
// function call on Release (no context switch, no second context). Measures the full per-stage
// cost of the structured continuation: construct + register + Release-with-Resume + destroy.
//
// This is the unit that replaces a context switch in a continuation CHAIN: N pipeline stages
// driven by N of these function-call dispatches plus a single awaiter resume, versus N context
// switches. Compare against the per-handoff cost in BM_PingPong_CoordinateWith (which is a full
// round trip of two context resumes).
//
static void BM_Continuation_Fire(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator coord;
        auto* co = ctx->GetCooperator();
        volatile uint64_t sink = 0;
        for (auto _ : state)
        {
            coord.TryAcquire(ctx);
            auto c = coord.Continue([&](coop::Coordinator*) { sink = sink + 1; return 0; });
            coord.Release(ctx, false /* schedule */);   // enqueues the continuation
            co->DrainContinuations();                   // fires it (function call, no switch)
        }
        benchmark::DoNotOptimize(sink);
    });
}
BENCHMARK(BM_Continuation_Fire);

// Detached (hoisted) continuation: self-owning, heap-allocated, no awaiter and no parked context.
// Measures the full per-fire cost INCLUDING the heap allocation and self-free — register (new) +
// Release (enqueue) + Drain (fire + delete). This is what a high-fan-out pipeline spends per stage
// with zero parked stacks. Compared against BM_Continuation_Fire (structured, frame-hosted, no
// heap) it isolates the allocation cost that the per-cooperator continuation pool targets.
//
static void BM_DetachedContinuation_Fire(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator coord;
        auto* co = ctx->GetCooperator();
        volatile uint64_t sink = 0;
        for (auto _ : state)
        {
            coord.TryAcquire(ctx);
            coord.ContinueDetached([&](coop::Coordinator*) { sink = sink + 1; });
            coord.Release(ctx, false /* schedule */);   // enqueues
            co->DrainContinuations();                   // fires + self-frees
        }
        benchmark::DoNotOptimize(sink);
    });
}
BENCHMARK(BM_DetachedContinuation_Fire);

// Full structured lifecycle on one context: register, fire (the continuation Resumes inline),
// then Await to collect the result. The Await here is non-blocking because the continuation has
// already fired (fired-before-await), so this isolates the continuation MACHINERY cost — the
// completion latch, the Await/Flash check, the result handoff and teardown — without a context
// switch. A real cross-context await (the continuation fires while the awaiter is blocked) adds
// exactly one context resume on top of this, which is the switch a chain amortizes away.
//
static void BM_Continuation_FireThenAwait(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator coord;
        auto* co = ctx->GetCooperator();
        volatile uint64_t sink = 0;
        for (auto _ : state)
        {
            coord.TryAcquire(ctx);
            auto c = coord.Continue([&](coop::Coordinator*) { sink = sink + 1; return 0; });
            coord.Release(ctx, false /* schedule */);   // enqueues
            co->DrainContinuations();                   // fire now, so Await is non-blocking
            benchmark::DoNotOptimize(c.Await());         // collect (already fired -> no switch)
        }
        benchmark::DoNotOptimize(sink);
    });
}
BENCHMARK(BM_Continuation_FireThenAwait);
