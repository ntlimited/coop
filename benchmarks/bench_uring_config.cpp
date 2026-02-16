#include <cassert>
#include <deque>
#include <functional>
#include <sys/socket.h>
#include <vector>

#include <benchmark/benchmark.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/thread.h"

#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"

// ---------------------------------------------------------------------------
// Uring configuration benchmarks
//
// Isolates the impact of io_uring setup flags on identical workloads. Each
// config variant runs the same benchmark body so differences reflect only
// the kernel-side flag behavior.
//
// Naming: BM_Uring_{Shape}_{Config}
//   Shape:  RoundTrip, PingPong
//   Config: Bare, CoopTaskrun, DeferTaskrun
// ---------------------------------------------------------------------------

static constexpr int MSG_SIZE = 64;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct BenchmarkArgs
{
    benchmark::State* state;
    std::function<void(coop::Context*, benchmark::State&)>* fn;
};

static void RunBenchmark(benchmark::State& state,
    std::function<void(coop::Context*, benchmark::State&)> fn,
    coop::CooperatorConfiguration const& config)
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

static void MakeSocketPair(int fds[2])
{
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(ret == 0);
    (void)ret;
}

// ---------------------------------------------------------------------------
// Configurations under test
// ---------------------------------------------------------------------------

// Bare: SINGLE_ISSUER only, no taskrun optimizations
//
static const coop::CooperatorConfiguration s_bare = {
    .uring = {
        .entries = 64,
        .registeredSlots = 64,
        .taskName = "Uring",
        .sqpoll = false,
        .iopoll = false,
        .coopTaskrun = false,
        .deferTaskrun = false,
    },
};

// CoopTaskrun: SINGLE_ISSUER + COOP_TASKRUN + TASKRUN_FLAG
//
static const coop::CooperatorConfiguration s_coopTaskrun = {
    .uring = {
        .entries = 64,
        .registeredSlots = 64,
        .taskName = "Uring",
        .sqpoll = false,
        .iopoll = false,
        .coopTaskrun = true,
        .deferTaskrun = false,
    },
};

// DeferTaskrun: SINGLE_ISSUER + DEFER_TASKRUN
//
static const coop::CooperatorConfiguration s_deferTaskrun = {
    .uring = {
        .entries = 64,
        .registeredSlots = 64,
        .taskName = "Uring",
        .sqpoll = false,
        .iopoll = false,
        .coopTaskrun = false,
        .deferTaskrun = true,
    },
};

// ---------------------------------------------------------------------------
// Shape: RoundTrip
//
// One context, one socketpair. Each iteration: Send + Recv. Measures raw
// uring IO overhead per syscall pair with no context switching.
// ---------------------------------------------------------------------------

static void RoundTripBody(coop::Context*, benchmark::State& state)
{
    int fds[2];
    MakeSocketPair(fds);

    coop::io::Descriptor a(fds[0]);
    coop::io::Descriptor b(fds[1]);

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};

    for (auto _ : state)
    {
        coop::io::Send(a, msg, MSG_SIZE);
        coop::io::Recv(b, buf, MSG_SIZE);
    }
}

static void BM_Uring_RoundTrip_Bare(benchmark::State& s)
    { RunBenchmark(s, RoundTripBody, s_bare); }
BENCHMARK(BM_Uring_RoundTrip_Bare);

static void BM_Uring_RoundTrip_CoopTaskrun(benchmark::State& s)
    { RunBenchmark(s, RoundTripBody, s_coopTaskrun); }
BENCHMARK(BM_Uring_RoundTrip_CoopTaskrun);

static void BM_Uring_RoundTrip_DeferTaskrun(benchmark::State& s)
    { RunBenchmark(s, RoundTripBody, s_deferTaskrun); }
BENCHMARK(BM_Uring_RoundTrip_DeferTaskrun);

// ---------------------------------------------------------------------------
// Shape: PingPong
//
// Two contexts, one socketpair. Ponger: Recv -> Send. Driver: Send -> Recv.
// Measures IO + context switching overhead.
// ---------------------------------------------------------------------------

static void PingPongBody(coop::Context* ctx, benchmark::State& state)
{
    int fds[2];
    MakeSocketPair(fds);

    coop::io::Descriptor a(fds[0]);

    bool done = false;
    bool pongerExited = false;

    ctx->GetCooperator()->Spawn([&done, &pongerExited, fd = fds[1]](coop::Context*)
    {
        coop::io::Descriptor b(fd);

        char buf[MSG_SIZE] = {};
        while (!done)
        {
            int r = coop::io::Recv(b, buf, MSG_SIZE);
            if (done || r <= 0) break;
            coop::io::Send(b, buf, r);
        }

        pongerExited = true;
    });

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};

    for (auto _ : state)
    {
        coop::io::Send(a, msg, MSG_SIZE);
        coop::io::Recv(a, buf, MSG_SIZE);
    }

    done = true;
    coop::io::Send(a, msg, MSG_SIZE);
    while (!pongerExited) ctx->Yield(true);
}

static void BM_Uring_PingPong_Bare(benchmark::State& s)
    { RunBenchmark(s, PingPongBody, s_bare); }
BENCHMARK(BM_Uring_PingPong_Bare);

static void BM_Uring_PingPong_CoopTaskrun(benchmark::State& s)
    { RunBenchmark(s, PingPongBody, s_coopTaskrun); }
BENCHMARK(BM_Uring_PingPong_CoopTaskrun);

static void BM_Uring_PingPong_DeferTaskrun(benchmark::State& s)
    { RunBenchmark(s, PingPongBody, s_deferTaskrun); }
BENCHMARK(BM_Uring_PingPong_DeferTaskrun);

// ---------------------------------------------------------------------------
// Shape: PingPong Scale
//
// N concurrent ponger contexts on N socketpairs. Driver round-robins.
// Captures how uring config interacts with descriptor/context pressure.
// ---------------------------------------------------------------------------

static void PingPongScaleBody(coop::Context* ctx, benchmark::State& state, int N)
{
    // deque: stable element addresses (no relocation on grow), required because Descriptor
    // is non-moveable (embedded list node).
    //
    std::deque<coop::io::Descriptor> writers;
    std::vector<int> readerFds(N);

    for (int i = 0; i < N; i++)
    {
        int fds[2];
        MakeSocketPair(fds);
        writers.emplace_back(fds[0]);
        readerFds[i] = fds[1];
    }

    bool done = false;
    int exited = 0;

    for (int i = 0; i < N; i++)
    {
        int fd = readerFds[i];
        ctx->GetCooperator()->Spawn([&done, &exited, fd](coop::Context*)
        {
            coop::io::Descriptor b(fd);

            char buf[MSG_SIZE] = {};
            while (!done)
            {
                int r = coop::io::Recv(b, buf, MSG_SIZE);
                if (done || r <= 0) break;
                coop::io::Send(b, buf, r);
            }

            ++exited;
        });
    }

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};

    for (auto _ : state)
    {
        for (int i = 0; i < N; i++)
        {
            coop::io::Send(writers[i], msg, MSG_SIZE);
            coop::io::Recv(writers[i], buf, MSG_SIZE);
        }
    }

    done = true;
    for (int i = 0; i < N; i++)
        coop::io::Send(writers[i], msg, MSG_SIZE);
    while (exited < N) ctx->Yield(true);
}

static void BM_Uring_PingPong_Scale_Bare(benchmark::State& s)
    { RunBenchmark(s, [&](coop::Context* c, benchmark::State& st)
        { PingPongScaleBody(c, st, s.range(0)); }, s_bare); }
BENCHMARK(BM_Uring_PingPong_Scale_Bare)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(256);

static void BM_Uring_PingPong_Scale_CoopTaskrun(benchmark::State& s)
    { RunBenchmark(s, [&](coop::Context* c, benchmark::State& st)
        { PingPongScaleBody(c, st, s.range(0)); }, s_coopTaskrun); }
BENCHMARK(BM_Uring_PingPong_Scale_CoopTaskrun)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(256);

static void BM_Uring_PingPong_Scale_DeferTaskrun(benchmark::State& s)
    { RunBenchmark(s, [&](coop::Context* c, benchmark::State& st)
        { PingPongScaleBody(c, st, s.range(0)); }, s_deferTaskrun); }
BENCHMARK(BM_Uring_PingPong_Scale_DeferTaskrun)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(256);
