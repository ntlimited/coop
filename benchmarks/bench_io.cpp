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

// Create a unix socketpair and return the raw fds. Descriptor constructors take ownership.
//
static void MakeSocketPair(int fds[2])
{
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(ret == 0);
    (void)ret;
}

// Factory for conditionally-registered Descriptors. C++17 mandatory copy elision guarantees the
// Descriptor is materialized directly at the caller's stack destination — no move/copy needed, and
// the `this` pointer inside the constructor is already the final address (so Push(this) is safe).
//
template<bool UseRegistered>
static coop::io::Descriptor MakeDescriptor(int fd)
{
    if constexpr (UseRegistered)
        return coop::io::Descriptor(coop::io::registered, fd);
    else
        return coop::io::Descriptor(fd);
}

static constexpr int MSG_SIZE = 64;

// ---------------------------------------------------------------------------
// 1. Single-context round-trip
// ---------------------------------------------------------------------------
//
// One context, one socketpair. Each iteration: Send on one end, Recv on the other. Measures raw
// uring IO overhead per syscall pair.
//

template<bool Reg>
static void BM_IO_RoundTrip_Impl(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context*, benchmark::State& state)
    {
        int fds[2];
        MakeSocketPair(fds);

        auto a = MakeDescriptor<Reg>(fds[0]);
        auto b = MakeDescriptor<Reg>(fds[1]);

        char msg[MSG_SIZE] = {};
        char buf[MSG_SIZE] = {};

        for (auto _ : state)
        {
            coop::io::Send(a, msg, MSG_SIZE);
            coop::io::Recv(b, buf, MSG_SIZE);
        }
    });
}

static void BM_IO_RoundTrip(benchmark::State& s) { BM_IO_RoundTrip_Impl<false>(s); }
BENCHMARK(BM_IO_RoundTrip);

static void BM_IO_RoundTrip_Registered(benchmark::State& s) { BM_IO_RoundTrip_Impl<true>(s); }
BENCHMARK(BM_IO_RoundTrip_Registered);

// ---------------------------------------------------------------------------
// 2. Two-context ping-pong
// ---------------------------------------------------------------------------
//
// Ponger child: loops Recv -> Send. Parent benchmark loop: Send -> Recv. Each iteration = 1 full
// round-trip through two contexts + two IO operations in each. Measures IO with context switching.
//

template<bool Reg>
static void BM_IO_PingPong_Impl(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeSocketPair(fds);

        auto a = MakeDescriptor<Reg>(fds[0]);

        bool done = false;
        bool pongerExited = false;

        ctx->GetCooperator()->Spawn([&done, &pongerExited, fd = fds[1]](coop::Context*)
        {
            auto b = MakeDescriptor<Reg>(fd);

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

        // Drain the ponger: unblock its Recv and wait for it to exit
        //
        done = true;
        coop::io::Send(a, msg, MSG_SIZE);
        while (!pongerExited) ctx->Yield(true);
    });
}

static void BM_IO_PingPong(benchmark::State& s) { BM_IO_PingPong_Impl<false>(s); }
BENCHMARK(BM_IO_PingPong);

static void BM_IO_PingPong_Registered(benchmark::State& s) { BM_IO_PingPong_Impl<true>(s); }
BENCHMARK(BM_IO_PingPong_Registered);

// ---------------------------------------------------------------------------
// 3. Scaled ping-pong
// ---------------------------------------------------------------------------
//
// Same two-context pattern but with N concurrent ponger contexts, each on its own socketpair.
// Parent drives all N pairs round-robin per iteration. Captures overhead of more
// descriptors/contexts in the system.
//

template<bool Reg>
static void BM_IO_PingPong_Scale_Impl(benchmark::State& state)
{
    const int N = state.range(0);

    RunBenchmark(state, [N](coop::Context* ctx, benchmark::State& state)
    {
        // deque for the writer side: stable element addresses (no relocation on grow), so
        // Descriptors can safely Push(this) into the uring's embedded list at construction time.
        //
        std::deque<coop::io::Descriptor> writers;

        // Raw fds for the reader side; ponger contexts create their own Descriptors
        //
        std::vector<int> readerFds(N);

        for (int i = 0; i < N; i++)
        {
            int fds[2];
            MakeSocketPair(fds);
            if constexpr (Reg)
                writers.emplace_back(coop::io::registered, fds[0]);
            else
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
                auto b = MakeDescriptor<Reg>(fd);

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

        // Drain all pongers
        //
        done = true;
        for (int i = 0; i < N; i++)
            coop::io::Send(writers[i], msg, MSG_SIZE);
        while (exited < N) ctx->Yield(true);
    });
}

static void BM_IO_PingPong_Scale(benchmark::State& s)
{
    BM_IO_PingPong_Scale_Impl<false>(s);
}
BENCHMARK(BM_IO_PingPong_Scale)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32);

static void BM_IO_PingPong_Scale_Registered(benchmark::State& s)
{
    BM_IO_PingPong_Scale_Impl<true>(s);
}
BENCHMARK(BM_IO_PingPong_Scale_Registered)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32);
