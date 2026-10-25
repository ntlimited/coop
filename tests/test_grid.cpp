#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coop/chan/channel.h"
#include "coop/context.h"
#include "coop/group.h"
#include "coop/io/handle.h"
#include "coop/io/timeout.h"
#include "coop/io/uring.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "coop/work/grid.h"

using namespace coop;
using Clock = std::chrono::steady_clock;

static inline uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
static inline void BusyFor(uint64_t ns)
{
    if (!ns) return;
    const uint64_t s = NowNs();
    volatile uint64_t x = 0;
    while (NowNs() - s < ns) for (int i = 0; i < 64; i++) x = x + (uint64_t)i;
    (void)x;
}

// Clustered shed (all Ergs shed from one cooperator -> land on one shard) must be drained by the
// whole grid via stealing: every Erg runs exactly once, and more than one cooperator participates.
//
TEST(GridTest, ShedBalancesAcrossCooperators)
{
    const int M = 4, N = 600;
    std::vector<Cooperator*> coops(M);
    std::vector<Thread*> threads(M);
    for (int m = 0; m < M; m++) { coops[m] = new Cooperator(); threads[m] = new Thread(coops[m]); }

    work::Grid grid;
    grid.Init(M);
    for (int m = 0; m < M; m++) grid.Join(coops[m]);

    std::atomic<int> remaining{N};
    std::vector<std::atomic<int>> ranOn(M);
    for (int m = 0; m < M; m++) ranOn[m].store(0, std::memory_order_relaxed);

    // Shed all N from cooperator 0; they cluster on shard 0 and must be stolen to balance.
    //
    coops[0]->Submit([&](Context*)
    {
        for (int i = 0; i < N; i++)
        {
            Shed([&]
            {
                BusyFor(5000);                               // heavy enough to force stealing
                Cooperator* me = GetCooperator();
                for (int m = 0; m < M; m++)
                    if (coops[m] == me) { ranOn[m].fetch_add(1, std::memory_order_relaxed); break; }
                remaining.fetch_sub(1, std::memory_order_acq_rel);
            });
        }
    });

    for (int spins = 0; remaining.load(std::memory_order_acquire) > 0 && spins < 200000; spins++)
        std::this_thread::sleep_for(std::chrono::microseconds(100));

    for (int m = 0; m < M; m++) coops[m]->Shutdown();
    for (int m = 0; m < M; m++) { delete threads[m]; delete coops[m]; }

    int total = 0, used = 0;
    for (int m = 0; m < M; m++) { int r = ranOn[m].load(std::memory_order_relaxed); total += r; if (r > 0) used++; }
    EXPECT_EQ(total, N) << "every shed Erg must run exactly once";
    EXPECT_GT(used, 1)  << "work-stealing must spread clustered Ergs across more than one cooperator";
}

// A caller-owned Erg (m_stealerOwned=false) shed via Shed(Erg*) -- the zero-allocation companion to
// Shed(fn) -- must be run by the grid and NOT freed by the stealer: the caller keeps ownership so a
// stable pipeline can re-shed the same object. We verify both the run and the no-free: the Erg sets
// a sentinel during Run, and after shutdown the caller still reads its state (a freed object would
// trip the debug guard pages). All Ergs are stack-resident in the test frame; none are allocated per
// shed.
//
TEST(GridTest, ShedReusableErgNotFreed)
{
    struct OwnedErg final : work::Erg
    {
        std::atomic<int> runs{0};
        OwnedErg() { m_stealerOwned = false; }
        void Run() final { runs.fetch_add(1, std::memory_order_relaxed); }
    };

    const int M = 4, N = 400;
    std::vector<Cooperator*> coops(M);
    std::vector<Thread*> threads(M);
    for (int m = 0; m < M; m++) { coops[m] = new Cooperator(); threads[m] = new Thread(coops[m]); }

    work::Grid grid;
    grid.Init(M);
    for (int m = 0; m < M; m++) grid.Join(coops[m]);

    std::vector<OwnedErg> ergs(N);
    std::atomic<int> remaining{N};

    coops[0]->Submit([&](Context*)
    {
        for (int i = 0; i < N; i++)
        {
            // Shed our own long-lived Erg; the stealer runs it without freeing it.
            //
            bool ok = Shed(&ergs[i]);
            EXPECT_TRUE(ok) << "Shed(Erg*) must accept a caller-owned Erg on a participating cooperator";
            (void)ok;
        }
    });

    // Drain: each Erg runs exactly once; count completions by polling the per-Erg sentinel.
    //
    for (int spins = 0; spins < 200000; spins++)
    {
        int done = 0;
        for (int i = 0; i < N; i++) if (ergs[i].runs.load(std::memory_order_relaxed) > 0) done++;
        if (done == N) break;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    for (int m = 0; m < M; m++) coops[m]->Shutdown();
    for (int m = 0; m < M; m++) { delete threads[m]; delete coops[m]; }

    // The caller still owns every Erg (not freed by the stealer) and each ran exactly once.
    //
    for (int i = 0; i < N; i++)
        EXPECT_EQ(ergs[i].runs.load(std::memory_order_relaxed), 1)
            << "caller-owned Erg " << i << " must run exactly once and survive (not be freed)";
}

// A burst larger than the bounded shard must execute every closure and release every capture.
// With a single cooperator, nothing can drain the shard until the producer yields.
//
TEST(GridTest, ClosureOverflowRunsInlineAndReleasesCaptures)
{
    constexpr int N = 9000; // exceeds the shard's 8192 slots
    std::vector<int> runs(N, 0);
    auto lifetime = std::make_shared<int>(0);
    int completed = 0;
    work::Grid grid;
    grid.Init(1);
    Cooperator co;
    {
        Thread thread(&co);
        grid.Join(&co);
        ASSERT_TRUE(co.Submit([&](Context* ctx)
        {
            for (int i = 0; i < N; ++i)
            {
                Shed([&, i, keep = lifetime]
                {
#ifndef NDEBUG
                    EXPECT_TRUE(coop::detail::InThunk());
#endif
                    ++runs[i];
                    ++completed;
                });
            }
            EXPECT_GT(completed, 0) << "overflow must run on the producer before it yields";
#ifndef NDEBUG
            EXPECT_FALSE(coop::detail::InThunk()) << "inline execution must restore thunk scope";
#endif
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (completed < N && Clock::now() < deadline)
            {
                ctx->Yield(true);
            }
            co.Shutdown();
        }));
    }
    EXPECT_EQ(completed, N);
    EXPECT_EQ(runs, std::vector<int>(N, 1)) << "each closure must run exactly once";
    EXPECT_EQ(lifetime.use_count(), 1) << "no closure captures may leak on overflow";
}

class GridFairnessTest : public ::testing::TestWithParam<bool> {};

// Keep a single shard continuously runnable by re-shedding its one Erg. The load stops on its
// finite run budget so a missing yield fails assertions instead of hanging the test process.
//
TEST_P(GridFairnessTest, SaturatedShardServicesContextsChannelsAndCqes)
{
    struct Load final : work::Erg
    {
        bool started = false;
        bool active = true;
        uint64_t startNs = 0;
        int runs = 0;
        Load() { m_stealerOwned = false; }
        void Run() final
        {
            if (!started)
            {
                started = true;
                startNs = NowNs();
            }
            ++runs;
            BusyFor(20000);
            if (runs >= 5000) // about 100ms of compute, with a nonempty shard until the final Run
            {
                active = false;
                return;
            }
            EXPECT_TRUE(Shed(this)); // no member accesses after publishing to the deque
        }
    } load;

    work::Grid grid;
    grid.Init(1);
    CooperatorConfiguration config = s_defaultCooperatorConfiguration;
    config.directYield = GetParam();
    Cooperator co(config);
    {
        Thread thread(&co);
        grid.Join(&co);
        ASSERT_TRUE(co.Submit([&](Context* ctx)
        {
            int buffer[1];
            chan::Channel<int> channel(ctx, buffer, 1);
            Group peers(ctx);
            int yields = 0, messages = 0, cqes = 0;
            uint64_t firstYieldUs = 0, firstChannelUs = 0, firstCqeUs = 0;
            EXPECT_TRUE(peers.Go([&](Context* peer)
            {
                while (!load.started) peer->Yield(true);
                for (int i = 0; i < 16; ++i)
                {
                    // A slow peer must not prevent subsequent service of the other contexts.
                    //
                    if (i == 0) BusyFor(2000000);
                    if (load.active)
                    {
                        if (++yields == 1) firstYieldUs = (NowNs() - load.startNs) / 1000;
                    }
                    peer->Yield(true);
                }
            }));
            EXPECT_TRUE(peers.Go([&](Context* peer)
            {
                while (!load.started) peer->Yield(true);
                for (int i = 0; i < 8; ++i)
                {
                    // An explicit io_uring timeout proves CQE reaping as well as context service.
                    //
                    Coordinator completion;
                    io::Handle timer(peer, GetUring(), &completion);
                    EXPECT_TRUE(io::Timeout(timer, std::chrono::milliseconds(1)));
                    EXPECT_EQ(timer.Wait(), -ETIME);
                    if (load.active)
                    {
                        if (++cqes == 1) firstCqeUs = (NowNs() - load.startNs) / 1000;
                    }
                }
            }));
            EXPECT_TRUE(peers.Go([&](Context* peer)
            {
                while (!load.started) peer->Yield(true);
                for (int i = 0; i < 16; ++i) EXPECT_TRUE(channel.Send(i));
            }));
            EXPECT_TRUE(peers.Go([&](Context* peer)
            {
                while (!load.started) peer->Yield(true);
                for (int i = 0; i < 16; ++i)
                {
                    int value = -1;
                    EXPECT_TRUE(channel.Recv(value));
                    EXPECT_EQ(value, i);
                    if (load.active)
                    {
                        if (++messages == 1) firstChannelUs = (NowNs() - load.startNs) / 1000;
                    }
                }
            }));
            EXPECT_TRUE(Shed(&load));
            while (load.active) ctx->Yield(true);
            EXPECT_TRUE(peers.Wait());
            EXPECT_GT(load.runs, 1);
            EXPECT_EQ(yields, 16) << "runnable contexts must run before the shed load ends";
            EXPECT_EQ(messages, 16) << "channel peers must exchange data while the shard stays busy";
            EXPECT_EQ(cqes, 8) << "io_uring completions must be reaped under sustained shed load";
            RecordProperty("first_context_us", static_cast<int>(firstYieldUs));
            RecordProperty("first_channel_us", static_cast<int>(firstChannelUs));
            RecordProperty("first_cqe_us", static_cast<int>(firstCqeUs));
            co.Shutdown();
        }));
    }
}

// With no runnable peer, stealer yields reach ReapOnly until the scheduler's batch ends.
// Queue the SQE inside the load so no setup poll can submit it; only the stealer's explicit
// Poll bounds submission and completion service to its quantum rather than a whole resume batch.
//
TEST_P(GridFairnessTest, LoneStealerServicesDeferredSqeWithinQuantumBound)
{
    struct Load final : work::Erg
    {
        io::Handle& op;
        Coordinator& completion;
        Coordinator& finished;
        int runs = 0;
        Clock::time_point submitted;
        Clock::time_point completed;
        Clock::duration longestRun{};

        Load(io::Handle& op, Coordinator& completion, Coordinator& finished)
            : op(op), completion(completion), finished(finished)
        {
            m_stealerOwned = false;
        }

        void Run() final
        {
            const auto start = Clock::now();
            EXPECT_EQ(GetCooperator()->YieldedCount(), 0u)
                << "only the stealer may be runnable during the saturated load";
            if (++runs == 1)
            {
                auto* sqe = GetUring()->GetSqe();
                EXPECT_NE(sqe, nullptr);
                if (!sqe)
                {
                    finished.Release(Self(), false);
                    return;
                }
                io_uring_prep_nop(sqe);
                submitted = Clock::now();
                op.Submit(sqe);
                EXPECT_TRUE(GetUring()->HasPendingSubmissions());
            }
            if (completed == Clock::time_point{} && !completion.IsHeld())
            {
                completed = Clock::now();
                EXPECT_EQ(op.Result(), 0);
            }
            constexpr auto morsel = work::detail::kStealerRunQuantum / 4;
            BusyFor(std::chrono::duration_cast<std::chrono::nanoseconds>(morsel).count());
            if (completed == Clock::time_point{})
            {
                longestRun = std::max(longestRun, Clock::now() - start);
            }
            if (runs < 128)
            {
                EXPECT_TRUE(Shed(this));
                return;
            }
            finished.Release(Self(), false);
        }
    };

    work::Grid grid;
    grid.Init(1);
    CooperatorConfiguration config = s_defaultCooperatorConfiguration;
    config.directYield = GetParam();
    Cooperator co(config);
    {
        Thread thread(&co);
        grid.Join(&co);
        ASSERT_TRUE(co.Submit([&](Context* ctx)
        {
            Coordinator completion, finished;
            io::Handle op(ctx, GetUring(), &completion);
            Load load(op, completion, finished);
            ASSERT_TRUE(finished.TryAcquire(ctx));
            ASSERT_TRUE(Shed(&load));
            finished.Acquire(ctx); // owner parks; no runnable peers or eager Handle::Wait poll

            EXPECT_NE(load.completed, Clock::time_point{})
                << "the deferred SQE must complete while the load is still saturating the shard";
            if (load.completed != Clock::time_point{})
            {
                // One quantum to submit, one to reap, plus two for dispatch/observation slack.
                // An indivisible Erg can overrun a quantum, so include its measured maximum.
                // The finite load lasts at least 32 quanta; draining it cannot satisfy this bound.
                //
                const auto bound = 4 * work::detail::kStealerRunQuantum + load.longestRun;
                EXPECT_LE(load.completed - load.submitted, bound);
                RecordProperty("lone_stealer_bound_us", static_cast<int>(
                    std::chrono::duration_cast<std::chrono::microseconds>(bound).count()));
                RecordProperty("lone_stealer_cqe_us", static_cast<int>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        load.completed - load.submitted).count()));
            }
            EXPECT_EQ(op.Wait(), 0); // drain even when the explicit-poll regression is present
            co.Shutdown();
        }));
    }
}

INSTANTIATE_TEST_SUITE_P(YieldModes, GridFairnessTest, ::testing::Bool());

// A yielding stealer services CQEs that can immediately re-shed hot pipelines. Those new stages
// must not continually overtake older queued work on the same shard.
//
TEST(GridTest, ReshedWorkDoesNotOvertakeQueuedWork)
{
    struct Hot final : work::Erg
    {
        int runs = 0;
        Hot() { m_stealerOwned = false; }
        void Run() final
        {
            ++runs;
            if (runs < 20) EXPECT_TRUE(Shed(this));
        }
    } hot;

    work::Grid grid;
    grid.Init(1);
    Cooperator co;
    {
        Thread thread(&co);
        grid.Join(&co);
        ASSERT_TRUE(co.Submit([&](Context* ctx)
        {
            int observedRuns = -1;
            Shed([&] { observedRuns = hot.runs; });
            EXPECT_TRUE(Shed(&hot));
            while (hot.runs < 20 || observedRuns < 0) ctx->Yield(true);
            EXPECT_GE(observedRuns, 0);
            EXPECT_LT(observedRuns, 20);
            co.Shutdown();
        }));
    }
}
