#include <benchmark/benchmark.h>

#include "coop/chan/channel.h"
#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/thread.h"

// ---------------------------------------------------------------------------
// Helper: run a benchmark body inside a cooperator
// ---------------------------------------------------------------------------

static void RunBenchmark(benchmark::State& state,
    std::function<void(coop::Context*, benchmark::State&)> fn)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    struct Args
    {
        benchmark::State* state;
        std::function<void(coop::Context*, benchmark::State&)>* fn;
    } args { &state, &fn };

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        auto* a = static_cast<Args*>(arg);
        (*a->fn)(ctx, *a->state);
        ctx->GetCooperator()->Shutdown();
    }, &args);
}

// ---------------------------------------------------------------------------
// BM_Channel_Uncontended — TrySend + TryRecv in the same context
// ---------------------------------------------------------------------------
//
// No blocking, no coordinator acquisition on the hot path (channel never becomes
// full or empty from the scheduler's perspective). Measures the ring buffer
// mechanics: m_size check, head/tail update, and the conditional coordinator
// management that doesn't fire.
//
static void BM_Channel_Uncontended(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        constexpr int CAP = 64;
        int buf[CAP];
        coop::chan::Channel<int> ch(ctx, buf, CAP);

        for (auto _ : state)
        {
            ch.TrySend(1);
            int v;
            ch.TryRecv(v);
            benchmark::DoNotOptimize(v);
        }
    });
}
BENCHMARK(BM_Channel_Uncontended);

// ---------------------------------------------------------------------------
// BM_Channel_PingPong — blocking send/recv between two contexts
// ---------------------------------------------------------------------------
//
// Capacity-1 channel so every Send blocks until the receiver consumes, and
// every Recv blocks until the sender produces. Each iteration = 2 context
// switches. Directly comparable to BM_PingPong_Raw in bench_coordinator.cpp —
// the gap is the channel layer's overhead on the blocking path.
//
static void BM_Channel_PingPong(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int buf[1];
        coop::chan::Channel<int> ch(ctx, buf, 1);

        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            int v;
            while (!done)
            {
                if (!ch.Recv(v)) break;
                benchmark::DoNotOptimize(v);
            }
        });

        for (auto _ : state)
        {
            ch.Send(1);
        }

        done = true;
        ch.Shutdown();
    });
}
BENCHMARK(BM_Channel_PingPong);

// ---------------------------------------------------------------------------
// BM_Channel_Throughput — items/sec vs buffer depth
// ---------------------------------------------------------------------------
//
// One producer and one consumer, both running for the duration of the
// benchmark. Measures how buffer depth trades off against context-switch
// frequency: a depth-1 channel switches on every item; deeper buffers allow
// the producer to fill ahead and amortize switch cost across a batch.
//
// SetItemsProcessed lets Google Benchmark report items/sec directly so runs
// with different iteration counts remain comparable.
//
static void BM_Channel_Throughput(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        const int cap = static_cast<int>(state.range(0));
        std::vector<int> buf(cap);
        coop::chan::Channel<int> ch(ctx, buf.data(), cap);

        // Fixed batch per outer iteration — large enough that the benchmark
        // loop overhead is negligible compared to the transfer work.
        //
        constexpr int BATCH = 4096;
        int64_t totalItems = 0;

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            while (true)
            {
                for (int i = 0; i < BATCH; i++)
                {
                    if (!ch.Send(i)) return;
                }
            }
        });

        for (auto _ : state)
        {
            for (int i = 0; i < BATCH; i++)
            {
                int v;
                ch.Recv(v);
                benchmark::DoNotOptimize(v);
            }
            totalItems += BATCH;
        }

        ch.Shutdown();
        state.SetItemsProcessed(totalItems);
    });
}
BENCHMARK(BM_Channel_Throughput)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(64);

// ---------------------------------------------------------------------------
// BM_Channel_Throughput_NProducers — throughput vs buffer depth with N producers
// ---------------------------------------------------------------------------
//
// With one producer and one consumer the throughput benchmark above is flat:
// schedule=true on every coordinator release means the consumer and producer
// alternate item-by-item regardless of buffer size. Buffer depth only helps
// when multiple producers can race ahead filling the channel concurrently
// while the consumer drains.
//
// This benchmark fixes buffer depth at 64 and sweeps producer count to show
// where multi-producer contention begins to benefit from a shared buffer.
//
static void BM_Channel_Throughput_NProducers(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        const int nProducers = static_cast<int>(state.range(0));
        constexpr int CAP = 64;
        int buf[CAP];
        coop::chan::Channel<int> ch(ctx, buf, CAP);

        constexpr int BATCH = 4096;
        int64_t totalItems = 0;

        for (int p = 0; p < nProducers; p++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context*)
            {
                while (true)
                {
                    for (int i = 0; i < BATCH; i++)
                    {
                        if (!ch.Send(i)) return;
                    }
                }
            });
        }

        for (auto _ : state)
        {
            for (int i = 0; i < BATCH; i++)
            {
                int v;
                ch.Recv(v);
                benchmark::DoNotOptimize(v);
            }
            totalItems += BATCH;
        }

        ch.Shutdown();
        state.SetItemsProcessed(totalItems);
    });
}
BENCHMARK(BM_Channel_Throughput_NProducers)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

// ---------------------------------------------------------------------------
// BM_Channel_SendAll_Drain — batch throughput vs buffer depth
// ---------------------------------------------------------------------------
//
// One producer calls SendAll(batch, BATCH), one consumer calls Drain(buf, BATCH).
// Unlike BM_Channel_Throughput which switches per-item, SendAll defers m_recv
// wakeup until the batch is complete (schedule=false on intermediate releases),
// and Drain defers m_send wakeup (schedule=false too). The result is that
// deeper buffers allow larger fills between switches, producing a real curve
// rather than the flat line seen with individual Send/Recv calls.
//
static void BM_Channel_SendAll_Drain(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        const int cap = static_cast<int>(state.range(0));
        std::vector<int> buf(cap);
        coop::chan::Channel<int> ch(ctx, buf.data(), cap);

        constexpr int BATCH = 4096;
        std::vector<int> sendBuf(BATCH);
        std::vector<int> recvBuf(BATCH);
        int64_t totalItems = 0;

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            while (true)
            {
                if (!ch.SendAll(sendBuf.data(), BATCH)) return;
            }
        });

        for (auto _ : state)
        {
            size_t drained = 0;
            while (drained < BATCH)
            {
                size_t n = ch.Drain(recvBuf.data() + drained, BATCH - drained);
                benchmark::DoNotOptimize(n);
                if (n == 0)
                {
                    int v;
                    ch.Recv(v);
                    recvBuf[drained++] = v;
                }
                else
                {
                    drained += n;
                }
            }
            totalItems += BATCH;
        }

        ch.Shutdown();
        state.SetItemsProcessed(totalItems);
    });
}
BENCHMARK(BM_Channel_SendAll_Drain)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(64);

// ---------------------------------------------------------------------------
// Pipeline benchmarks — multi-stage channel chains
// ---------------------------------------------------------------------------
//
// Each stage spawns a context that loops on Drain+transform+SendAll. The outer
// context is the producer/consumer. Stages increment each value by 1 (keeps
// the transform cost negligible so results reflect channel overhead).
//
// Shutdown propagates from the first channel outward: when a stage's input is
// shut down it shuts down its output, eventually reaching the consumer.
//

// Run one stage for the duration of the benchmark.
//
static void RunPipelineStage(coop::chan::Channel<int>& in, coop::chan::Channel<int>& out, int batchSize)
{
    std::vector<int> buf(batchSize);

    while (true)
    {
        size_t n = in.Drain(buf.data(), batchSize);
        if (n == 0)
        {
            int v;
            if (!in.Recv(v)) break;
            buf[0] = v + 1;
            n = 1;
        }
        else
        {
            for (size_t i = 0; i < n; i++) buf[i]++;
        }

        if (!out.SendAll(buf.data(), n)) break;
    }

    out.Shutdown();
}

// ---------------------------------------------------------------------------
// BM_Channel_Pipeline_NStages — throughput vs stage count
// ---------------------------------------------------------------------------
//
// Fixed channel depth (16). Sweep stage count from 1 to 8. Each additional
// stage adds one context switch per batch boundary, so items/sec drops
// roughly linearly with stage count. The slope is the per-stage cost.
//
static void BM_Channel_Pipeline_NStages(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        const int nStages = static_cast<int>(state.range(0));
        constexpr int DEPTH = 16;
        constexpr int BATCH = 1024;

        std::vector<std::vector<int>> bufs(nStages + 1, std::vector<int>(DEPTH));
        std::vector<std::unique_ptr<coop::chan::Channel<int>>> chs;
        for (int i = 0; i <= nStages; i++)
            chs.push_back(std::make_unique<coop::chan::Channel<int>>(ctx, bufs[i].data(), DEPTH));

        for (int s = 0; s < nStages; s++)
        {
            ctx->GetCooperator()->Spawn([&chs, s](coop::Context*)
            {
                RunPipelineStage(*chs[s], *chs[s + 1], DEPTH);
            });
        }

        ctx->GetCooperator()->Spawn([&chs](coop::Context*)
        {
            std::vector<int> buf(BATCH);
            for (int i = 0; i < BATCH; i++) buf[i] = i;
            while (chs[0]->SendAll(buf.data(), BATCH)) {}
        });

        int64_t totalItems = 0;
        std::vector<int> drain(BATCH);

        for (auto _ : state)
        {
            size_t got = 0;
            while (got < (size_t)BATCH)
            {
                size_t n = chs.back()->Drain(drain.data() + got, BATCH - got);
                if (n == 0)
                {
                    int v;
                    chs.back()->Recv(v);
                    got++;
                }
                else
                {
                    got += n;
                }
                benchmark::DoNotOptimize(got);
            }
            totalItems += BATCH;
        }

        chs[0]->Shutdown();
        { int v; while (chs.back()->Recv(v)) {} }

        state.SetItemsProcessed(totalItems);
    });
}
BENCHMARK(BM_Channel_Pipeline_NStages)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

// ---------------------------------------------------------------------------
// BM_Channel_Pipeline_Depth — throughput vs buffer depth (fixed 4 stages)
// ---------------------------------------------------------------------------
//
// Fixed 4 stages. Sweep channel depth from 1 to 64. Deeper buffers allow
// each stage to fill and drain larger batches before switching, amortizing
// the per-switch cost across more items. Unlike the single-producer throughput
// benchmark (which is flat because one producer can't race ahead), a pipeline
// should show increasing throughput with depth — stages overlap their work.
//
static void BM_Channel_Pipeline_Depth(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        constexpr int N_STAGES = 4;
        const int depth = static_cast<int>(state.range(0));
        const int batch = std::max(depth * 4, 256);

        std::vector<std::vector<int>> bufs(N_STAGES + 1, std::vector<int>(depth));
        std::vector<std::unique_ptr<coop::chan::Channel<int>>> chs;
        for (int i = 0; i <= N_STAGES; i++)
            chs.push_back(std::make_unique<coop::chan::Channel<int>>(ctx, bufs[i].data(), depth));

        for (int s = 0; s < N_STAGES; s++)
        {
            ctx->GetCooperator()->Spawn([&chs, s, depth](coop::Context*)
            {
                RunPipelineStage(*chs[s], *chs[s + 1], depth);
            });
        }

        ctx->GetCooperator()->Spawn([&chs, batch](coop::Context*)
        {
            std::vector<int> buf(batch);
            for (int i = 0; i < batch; i++) buf[i] = i;
            while (chs[0]->SendAll(buf.data(), batch)) {}
        });

        int64_t totalItems = 0;
        std::vector<int> drain(batch);

        for (auto _ : state)
        {
            size_t got = 0;
            while (got < (size_t)batch)
            {
                size_t n = chs.back()->Drain(drain.data() + got, batch - got);
                if (n == 0)
                {
                    int v;
                    chs.back()->Recv(v);
                    got++;
                }
                else
                {
                    got += n;
                }
                benchmark::DoNotOptimize(got);
            }
            totalItems += batch;
        }

        chs[0]->Shutdown();
        { int v; while (chs.back()->Recv(v)) {} }

        state.SetItemsProcessed(totalItems);
    });
}
BENCHMARK(BM_Channel_Pipeline_Depth)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(64);
