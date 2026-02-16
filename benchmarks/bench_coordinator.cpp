#include <functional>

#include <benchmark/benchmark.h>

#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "coop/time/interval.h"

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
// Uncontended: single context, fast paths
// ---------------------------------------------------------------------------

// Raw TryAcquire + Release on an unheld coordinator. No context switching, no kill signal.
//
static void BM_AcquireRelease(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator coord;
        for (auto _ : state)
        {
            coord.TryAcquire(ctx);
            coord.Release(ctx, false);
        }
    });
}
BENCHMARK(BM_AcquireRelease);

// CoordinateWith on one unheld coordinator. Pure multi-coordinator path, no kill signal.
//
static void BM_CoordinateWith_1Coord(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator coord;
        for (auto _ : state)
        {
            auto result = coop::CoordinateWith(ctx, &coord);
            benchmark::DoNotOptimize(result.coordinator);
            coord.Release(ctx, false);
        }
    });
}
BENCHMARK(BM_CoordinateWith_1Coord);

// CoordinateWithKill on one unheld coordinator. Measures the overhead of kill-signal integration
// (MultiCoordinator try kill -> fail/arm -> try user coord -> succeed -> undo kill hookup) versus
// raw CoordinateWith.
//
static void BM_CoordinateWithKill_1Coord(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator coord;
        for (auto _ : state)
        {
            auto result = coop::CoordinateWithKill(ctx, &coord);
            benchmark::DoNotOptimize(result.coordinator);
            coord.Release(ctx, false);
        }
    });
}
BENCHMARK(BM_CoordinateWithKill_1Coord);

// CoordinateWith with two unheld coordinators. Measures MultiCoordinator scaling, no kill signal.
//
static void BM_CoordinateWith_2Coords(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator c1, c2;
        for (auto _ : state)
        {
            auto result = coop::CoordinateWith(ctx, &c1, &c2);
            benchmark::DoNotOptimize(result.coordinator);
            result.coordinator->Release(ctx, false);
        }
    });
}
BENCHMARK(BM_CoordinateWith_2Coords);

// CoordinateWithKill with two unheld coordinators. Kill signal + two user coordinators.
//
static void BM_CoordinateWithKill_2Coords(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator c1, c2;
        for (auto _ : state)
        {
            auto result = coop::CoordinateWithKill(ctx, &c1, &c2);
            benchmark::DoNotOptimize(result.coordinator);
            result.coordinator->Release(ctx, false);
        }
    });
}
BENCHMARK(BM_CoordinateWithKill_2Coords);

// CoordinateWith with one unheld coordinator + a timeout that never fires. Measures timer
// setup + cancel overhead on the fast path.
//
static void BM_CoordinateWith_Timeout_NoFire(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator coord;
        for (auto _ : state)
        {
            auto result = coop::CoordinateWith(ctx, &coord, std::chrono::milliseconds(1000));
            benchmark::DoNotOptimize(result.coordinator);
            coord.Release(ctx, false);
        }
    });
}
BENCHMARK(BM_CoordinateWith_Timeout_NoFire);

// CoordinateWithKill with one unheld coordinator + a timeout that never fires.
//
static void BM_CoordinateWithKill_Timeout_NoFire(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator coord;
        for (auto _ : state)
        {
            auto result = coop::CoordinateWithKill(
                ctx, &coord, std::chrono::milliseconds(1000));
            benchmark::DoNotOptimize(result.coordinator);
            coord.Release(ctx, false);
        }
    });
}
BENCHMARK(BM_CoordinateWithKill_Timeout_NoFire);

// ---------------------------------------------------------------------------
// Contended: two contexts, ping-pong context switching
// ---------------------------------------------------------------------------
//
// Parent holds c1 and c2. Child blocks on c1, parent releases c1 (switches to child), child
// releases c1 then blocks on c2, parent releases c2 (switches to child), child releases c2 then
// loops back. Each benchmark iteration = 2 context switches.
//

// Ping-pong using raw Acquire/Release.
//
static void BM_PingPong_Raw(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator c1, c2;
        c1.TryAcquire(ctx);
        c2.TryAcquire(ctx);

        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            while (!done)
            {
                c1.Acquire(child);
                c1.Release(child, false);
                if (done) break;
                c2.Acquire(child);
                c2.Release(child, false);
            }
        });

        // Child is now blocked on c1.Acquire
        //
        for (auto _ : state)
        {
            c1.Release(ctx, true);
            c1.TryAcquire(ctx);
            c2.Release(ctx, true);
            c2.TryAcquire(ctx);
        }

        // Let the child exit
        //
        done = true;
        c1.Release(ctx, true);
    });
}
BENCHMARK(BM_PingPong_Raw);

// Same ping-pong but child uses CoordinateWith (no kill signal) instead of raw Acquire.
//
static void BM_PingPong_CoordinateWith(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator c1, c2;
        c1.TryAcquire(ctx);
        c2.TryAcquire(ctx);

        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            while (!done)
            {
                auto r = coop::CoordinateWith(child, &c1);
                c1.Release(child, false);
                if (done) break;
                r = coop::CoordinateWith(child, &c2);
                c2.Release(child, false);
            }
        });

        // Child is now blocked inside CoordinateWith on c1
        //
        for (auto _ : state)
        {
            c1.Release(ctx, true);
            c1.TryAcquire(ctx);
            c2.Release(ctx, true);
            c2.TryAcquire(ctx);
        }

        // Let the child exit
        //
        done = true;
        c1.Release(ctx, true);
    });
}
BENCHMARK(BM_PingPong_CoordinateWith);

// Same ping-pong but child uses CoordinateWithKill (kill signal included). Measures the overhead
// of kill-signal integration in the blocking (slow) path.
//
static void BM_PingPong_CoordinateWithKill(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        coop::Coordinator c1, c2;
        c1.TryAcquire(ctx);
        c2.TryAcquire(ctx);

        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            while (!done)
            {
                auto r = coop::CoordinateWithKill(child, &c1);
                c1.Release(child, false);
                if (done) break;
                r = coop::CoordinateWithKill(child, &c2);
                c2.Release(child, false);
            }
        });

        // Child is now blocked inside CoordinateWithKill on c1
        //
        for (auto _ : state)
        {
            c1.Release(ctx, true);
            c1.TryAcquire(ctx);
            c2.Release(ctx, true);
            c2.TryAcquire(ctx);
        }

        // Let the child exit
        //
        done = true;
        c1.Release(ctx, true);
    });
}
BENCHMARK(BM_PingPong_CoordinateWithKill);

// ---------------------------------------------------------------------------
// Kill signal
// ---------------------------------------------------------------------------

// Spawn a child that blocks on a coordinator via CoordinateWithKill, immediately kill it, yield
// to let cleanup run. Measures the full spawn + kill + cleanup cycle.
//
static void BM_SpawnAndKill(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        for (auto _ : state)
        {
            coop::Coordinator coord(ctx);
            coop::Context::Handle handle;

            ctx->GetCooperator()->Spawn([&](coop::Context* child)
            {
                auto result = coop::CoordinateWithKill(child, &coord);
                benchmark::DoNotOptimize(result.Killed());
            }, &handle);

            // Child is blocked inside CoordinateWithKill. Kill it.
            //
            handle.Kill();

            // Yield to let the killed child run its cleanup and exit.
            //
            ctx->Yield(true);

            coord.Release(ctx, false);
        }
    });
}
BENCHMARK(BM_SpawnAndKill);

// ---------------------------------------------------------------------------
// Timeout fires
// ---------------------------------------------------------------------------

// CoordinateWithKill with a held coordinator + timeout that actually fires. Measures the real
// timeout path including timer firing and wakeup.
//
static void BM_CoordinateWithKill_Timeout_Fires(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        for (auto _ : state)
        {
            coop::Coordinator coord(ctx);
            auto result = coop::CoordinateWithKill(
                ctx, &coord, std::chrono::milliseconds(1));
            benchmark::DoNotOptimize(result.TimedOut());
            coord.Release(ctx, false);
        }
    });
}
BENCHMARK(BM_CoordinateWithKill_Timeout_Fires);
