#include <cstdint>
#include <cstdio>
#include <vector>

#include "coop/chan/channel.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/thread.h"

using namespace coop;
using namespace coop::chan;

// ---------------------------------------------------------------------------
// Channel pipeline example
//
// Demonstrates chaining Channel<int64_t> stages where each stage drains its
// input in batches (Drain), transforms values, and pushes to the next stage
// (SendAll). Shutdown propagates naturally: when a stage's input is exhausted
// it shuts down its output, signalling the next stage to do the same.
//
//   Source ──ch0──► [×2] ──ch1──► [+100] ──ch2──► Sink
//
// ---------------------------------------------------------------------------

static constexpr int64_t N     = 100'000;
static constexpr int     DEPTH = 64;    // channel buffer depth
static constexpr int     BATCH = 256;   // stage working batch size

// Run one pipeline stage: drain from `in`, apply `fn` to each item, send to
// `out`. Shuts down `out` when `in` is exhausted.
//
template<typename Fn>
static void RunStage(Channel<int64_t>& in, Channel<int64_t>& out, Fn fn)
{
    std::vector<int64_t> buf(BATCH);

    while (true)
    {
        size_t n = in.Drain(buf.data(), BATCH);
        if (n == 0)
        {
            int64_t v;
            if (!in.Recv(v)) break;
            buf[0] = fn(v);
            n = 1;
        }
        else
        {
            for (size_t i = 0; i < n; i++)
                buf[i] = fn(buf[i]);
        }

        if (!out.SendAll(buf.data(), n)) break;
    }

    out.Shutdown();
}

int main()
{
    Cooperator co;
    Thread t(&co);

    co.Submit([](Context* ctx, void*)
    {
        Cooperator* co = ctx->GetCooperator();

        std::vector<int64_t> b0(DEPTH), b1(DEPTH), b2(DEPTH);
        Channel<int64_t> ch0(ctx, b0.data(), DEPTH);
        Channel<int64_t> ch1(ctx, b1.data(), DEPTH);
        Channel<int64_t> ch2(ctx, b2.data(), DEPTH);

        // Stage 1: multiply by 2
        //
        co->Spawn([&](Context*) { RunStage(ch0, ch1, [](int64_t v) { return v * 2; }); });

        // Stage 2: add 100
        //
        co->Spawn([&](Context*) { RunStage(ch1, ch2, [](int64_t v) { return v + 100; }); });

        // Source: push 0..N-1 in batches, then shut down.
        //
        co->Spawn([&](Context*)
        {
            std::vector<int64_t> src(BATCH);
            for (int64_t base = 0; base < N; base += BATCH)
            {
                int64_t count = std::min((int64_t)BATCH, N - base);
                for (int64_t i = 0; i < count; i++)
                    src[i] = base + i;
                if (!ch0.SendAll(src.data(), count)) return;
            }
            ch0.Shutdown();
        });

        // Sink: drain ch2 and verify.
        //
        int64_t sum   = 0;
        int64_t count = 0;
        {
            std::vector<int64_t> buf(BATCH);
            while (true)
            {
                size_t n = ch2.Drain(buf.data(), BATCH);
                if (n == 0)
                {
                    int64_t v;
                    if (!ch2.Recv(v)) break;
                    sum += v;
                    count++;
                }
                else
                {
                    for (size_t i = 0; i < n; i++) sum += buf[i];
                    count += (int64_t)n;
                }
            }
        }

        // Expected: sum(2i + 100 for i = 0..N-1)
        //         = 2 * N*(N-1)/2 + 100*N
        //         = N*(N-1) + 100*N
        //         = N * (N + 99)
        //
        int64_t expected = N * (N + 99);

        printf("Items: %lld (expected %lld) %s\n",
            (long long)count, (long long)N, count == N ? "OK" : "MISMATCH");
        printf("Sum:   %lld (expected %lld) %s\n",
            (long long)sum, (long long)expected, sum == expected ? "OK" : "MISMATCH");

        co->Shutdown();
    }, nullptr);

    return 0;
}
