#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "coop/context.h"
#include "coop/continuation.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/self.h"
#include "coop/thread.h"

// ---------------------------------------------------------------------------
// High fan-out: N concurrent IO-driven pipelines, each advanced one stage per
// iteration. This is the scenario the detached-continuation hoist was built for.
// Two ways to keep N operations in flight:
//
//   Context-per-pipeline  — N contexts each parked (CoordinateWithKill) on their
//                           coordinator awaiting the next completion. Cost: N parked
//                           16KB stacks, and one context switch per advance per pipeline.
//   Detached-per-pipeline — N detached continuations, each registered on its
//                           coordinator, no context parked. Cost: N ~128-byte pool
//                           nodes, and one function-call fire per advance per pipeline.
//
// "Advancing one stage" models one round of completions: release every coordinator
// (the op completed) and let each pipeline run its handler and re-arm for the next op.
// The benchmark reports items/s (pipelines advanced per second) and bytes/pipe (the
// per-in-flight-pipeline memory footprint) so the time AND the memory axes are visible.
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

// Detached: each pipeline is a self-re-arming detached continuation. Firing increments the
// pipeline's counter and (unless stopping) re-acquires the coordinator and registers the next
// stage. No context is parked anywhere.
//
static void BM_FanOut_AdvanceStage_Detached(benchmark::State& state)
{
    const int N = static_cast<int>(state.range(0));
    RunBenchmark(state, [N](coop::Context* ctx, benchmark::State& state)
    {
        auto* co = ctx->GetCooperator();
        std::unique_ptr<coop::Coordinator[]> coords(new coop::Coordinator[N]);
        std::vector<uint64_t> counters(N, 0);
        bool stop = false;

        struct Stage
        {
            coop::Coordinator* coord;
            uint64_t*          counter;
            const bool*        stop;

            void operator()(coop::Coordinator*) const
            {
                ++*counter;
                if (!*stop)
                {
                    coord->TryAcquire();
                    coord->ContinueDetached(*this);     // re-arm for the next completion
                }
            }
        };

        for (int i = 0; i < N; ++i)
        {
            coords[i].TryAcquire();
            coords[i].ContinueDetached(Stage{&coords[i], &counters[i], &stop});
        }

        for (auto _ : state)
        {
            for (int i = 0; i < N; ++i)
            {
                coords[i].Release(nullptr, /*schedule=*/false);
            }
            co->DrainContinuations();                   // fire all N (increment + re-arm)
        }

        // Drain the last armed round without re-arming so every continuation self-frees and the
        // coordinators end empty (the ~Coordinator leak assert holds).
        //
        stop = true;
        for (int i = 0; i < N; ++i)
        {
            coords[i].Release(nullptr, /*schedule=*/false);
        }
        co->DrainContinuations();

        state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(N));
        state.counters["bytes/pipe"] = sizeof(coop::DetachedContinuationImpl<Stage>);
        state.counters["pipes"] = N;
    });
}
BENCHMARK(BM_FanOut_AdvanceStage_Detached)->RangeMultiplier(8)->Range(64, 4096)->UseRealTime();

// Context: each pipeline is a context parked on its coordinator via CoordinateWithKill (kill-aware
// so Shutdown unwinds them). Advancing releases every coordinator and yields once; the cooperator
// loop resumes all N workers (each increments and re-blocks) before control returns here.
//
static void BM_FanOut_AdvanceStage_Context(benchmark::State& state)
{
    const int N = static_cast<int>(state.range(0));
    RunBenchmark(state, [N](coop::Context* ctx, benchmark::State& state)
    {
        std::unique_ptr<coop::Coordinator[]> coords(new coop::Coordinator[N]);
        std::vector<uint64_t> counters(N, 0);
        bool stop = false;
        int  live = N;

        for (int i = 0; i < N; ++i)
        {
            coords[i].TryAcquire();                     // held: the worker's first wait blocks
            coop::Coordinator* c = &coords[i];
            uint64_t* cnt = &counters[i];
            coop::Spawn([c, cnt, &stop, &live](coop::Context* w)
            {
                // Plain (not kill-aware) wait: the single-coordinator fast path, the cheapest
                // context block coop has. Teardown wakes the worker via the stop flag, so kill
                // awareness is unnecessary here and would only add multiplex overhead.
                //
                while (true)
                {
                    coop::CoordinateWith(w, c);
                    if (stop)
                    {
                        --live;
                        break;
                    }
                    ++*cnt;
                }
            });
        }

        ctx->Yield();                                   // let all N workers reach their first block

        for (auto _ : state)
        {
            for (int i = 0; i < N; ++i)
            {
                coords[i].Release(nullptr, /*schedule=*/false);
            }
            ctx->Yield();                               // run all N workers (advance + re-block)
        }

        state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(N));
        state.counters["bytes/pipe"] = coop::s_defaultConfiguration.stackSize;
        state.counters["pipes"] = N;

        // Stop the workers and let them unwind before coords destructs (a coordinator with a
        // worker still parked on it would trip the ~Coordinator leak assert).
        //
        stop = true;
        for (int i = 0; i < N; ++i)
        {
            coords[i].Release(nullptr, /*schedule=*/false);
        }
        while (live > 0)
        {
            ctx->Yield();
        }
    });
}
BENCHMARK(BM_FanOut_AdvanceStage_Context)->RangeMultiplier(8)->Range(64, 4096)->UseRealTime();
