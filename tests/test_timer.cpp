#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"
#include "coop/time/timer_queue.h"
#include "test_helpers.h"

using namespace std::chrono_literals;

// Run a test body inside a cooperator configured with a specific TimerMode. The integration tests
// run their assertions under BOTH modes so the userspace queue is proven to match the proven
// kernel-per-timer path, not merely to pass on its own.
//
static void RunWithTimerMode(coop::TimerMode mode, std::function<void(coop::Context*)> fn)
{
    coop::CooperatorConfiguration cfg;
    cfg.timerMode = mode;
    coop::Cooperator cooperator(cfg);
    coop::Thread t(&cooperator);

    cooperator.Submit([&fn](coop::Context* ctx)
    {
        fn(ctx);
        ctx->GetCooperator()->Shutdown();
    });
}

// ---------------------------------------------------------------------------
// TimerQueue unit tests: the structure in isolation, no cooperator.
// ---------------------------------------------------------------------------

// PopExpired returns nodes in non-decreasing deadline order, and never returns a node whose
// deadline is past the supplied 'now'.
//
TEST(TimerQueueTest, PopsInDeadlineOrder)
{
    coop::time::TimerQueue q;
    coop::Coordinator dummy;

    std::vector<int64_t> deadlines = {500, 100, 900, 100, 300, 700, 200, 100, 800, 400};
    std::vector<coop::time::TimerNode> nodes(deadlines.size());
    for (size_t i = 0; i < deadlines.size(); i++)
    {
        q.Insert(&nodes[i], deadlines[i], &dummy);
        ASSERT_TRUE(q.Validate());
    }

    EXPECT_FALSE(q.Empty());
    EXPECT_EQ(q.MinDeadlineUs(), 100);

    std::vector<int64_t> popped;
    while (auto* n = q.PopExpired(1'000'000))
    {
        popped.push_back(n->DeadlineUs());
        ASSERT_TRUE(q.Validate());
    }
    EXPECT_TRUE(q.Empty());

    auto sorted = deadlines;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(popped, sorted);
}

// PopExpired respects the 'now' cutoff: only deadlines at or before it come out.
//
TEST(TimerQueueTest, PopExpiredRespectsCutoff)
{
    coop::time::TimerQueue q;
    coop::Coordinator dummy;

    std::vector<coop::time::TimerNode> nodes(5);
    int64_t deadlines[5] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++)
    {
        q.Insert(&nodes[i], deadlines[i], &dummy);
    }

    int count = 0;
    while (q.PopExpired(25)) count++;
    EXPECT_EQ(count, 2);                 // only 10 and 20 are <= 25
    EXPECT_EQ(q.MinDeadlineUs(), 30);

    while (q.PopExpired(1000)) count++;
    EXPECT_EQ(count, 5);
    EXPECT_TRUE(q.Empty());
}

// Arbitrary Remove keeps the heap valid and the min correct, and is idempotent on an already
// removed node (the cleanup-path contract).
//
TEST(TimerQueueTest, ArbitraryRemove)
{
    coop::time::TimerQueue q;
    coop::Coordinator dummy;

    std::vector<coop::time::TimerNode> nodes(8);
    for (int i = 0; i < 8; i++)
    {
        q.Insert(&nodes[i], (i + 1) * 100, &dummy);
    }

    q.Remove(&nodes[0]);                 // remove the minimum (deadline 100)
    EXPECT_TRUE(q.Validate());
    EXPECT_EQ(q.MinDeadlineUs(), 200);

    q.Remove(&nodes[4]);                 // remove an interior node (deadline 500)
    EXPECT_TRUE(q.Validate());

    q.Remove(&nodes[4]);                 // idempotent: already unlinked
    EXPECT_TRUE(q.Validate());
    EXPECT_FALSE(nodes[4].Linked());

    int remaining = 0;
    while (q.PopExpired(1'000'000)) remaining++;
    EXPECT_EQ(remaining, 6);
}

// Stress: random interleaving of inserts and removes keeps the heap invariant, and a final drain
// yields exactly the still-linked nodes in deadline order.
//
TEST(TimerQueueTest, RandomizedStress)
{
    coop::time::TimerQueue q;
    coop::Coordinator dummy;

    constexpr int N = 400;
    std::vector<coop::time::TimerNode> nodes(N);
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int64_t> deadlineDist(0, 100000);

    std::vector<int> linked;
    for (int i = 0; i < N; i++)
    {
        q.Insert(&nodes[i], deadlineDist(rng), &dummy);
        linked.push_back(i);

        // Occasionally remove a random currently-linked node.
        //
        if ((i & 3) == 0 && !linked.empty())
        {
            std::uniform_int_distribution<size_t> pick(0, linked.size() - 1);
            size_t idx = pick(rng);
            q.Remove(&nodes[linked[idx]]);
            linked.erase(linked.begin() + idx);
        }
        ASSERT_TRUE(q.Validate());
    }

    int64_t prev = -1;
    int drained = 0;
    while (auto* n = q.PopExpired(1'000'000))
    {
        EXPECT_GE(n->DeadlineUs(), prev);    // non-decreasing
        prev = n->DeadlineUs();
        drained++;
    }
    EXPECT_EQ(drained, static_cast<int>(linked.size()));
}

// ---------------------------------------------------------------------------
// Integration: sleeps running on a real cooperator.
// ---------------------------------------------------------------------------

// The integration tests run under both timer modes via a value-parameterized fixture.
//
class TimerIntegrationTest : public ::testing::TestWithParam<coop::TimerMode> {};

INSTANTIATE_TEST_SUITE_P(
    Modes, TimerIntegrationTest,
    ::testing::Values(coop::TimerMode::KernelPerTimer, coop::TimerMode::UserspaceQueue),
    [](const ::testing::TestParamInfo<coop::TimerMode>& info)
    {
        return info.param == coop::TimerMode::UserspaceQueue ? "UserspaceQueue" : "KernelPerTimer";
    });

// Many concurrent sleeps each wake no earlier than their requested interval, and complete in
// deadline order. This is the fan-out the timer queue exists to serve.
//
TEST_P(TimerIntegrationTest, ConcurrentSleepsFireInOrderNotEarly)
{
    RunWithTimerMode(GetParam(), [](coop::Context* ctx)
    {
        constexpr int N = 64;

        struct Record { int id; int64_t requestedUs; int64_t elapsedUs; };
        std::vector<Record> done;
        done.reserve(N);

        int completed = 0;

        for (int i = 0; i < N; i++)
        {
            // Spread deadlines across a window wide enough that the cooperator genuinely idles
            // between expiries (so the single-timer arm/service path is exercised), with several
            // sharing a deadline to exercise equal keys.
            //
            int64_t requestedUs = 2000 + (i % 32) * 1000;

            ctx->GetCooperator()->Spawn([&, i, requestedUs](coop::Context* child)
            {
                auto t0 = std::chrono::steady_clock::now();
                auto r = coop::time::Sleep(child, std::chrono::microseconds(requestedUs));
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                if (r == coop::time::SleepResult::Ok)
                {
                    done.push_back({i, requestedUs, elapsed});
                }
                completed++;
            });
        }

        while (completed < N)
        {
            ctx->Yield(true);
        }

        ASSERT_EQ(static_cast<int>(done.size()), N);

        // One-sided covenant: no sleep returns before its requested interval.
        //
        for (auto& r : done)
        {
            EXPECT_GE(r.elapsedUs, r.requestedUs)
                << "sleep " << r.id << " fired early";
        }

        // Completion order follows deadline order: each completion's requested interval is >= the
        // previous one's (the queue services nearest-deadline-first).
        //
        for (size_t k = 1; k < done.size(); k++)
        {
            EXPECT_GE(done[k].requestedUs, done[k - 1].requestedUs)
                << "out-of-order completion at index " << k;
        }
    });
}

// A mix of exact and slack sleeps: every sleep still wakes no earlier than its requested interval,
// and a slack sleep overshoots by at most ~one slack tick. Exact and slack deadlines coexist in one
// queue without disturbing each other.
//
TEST_P(TimerIntegrationTest, MixedSlackAndExact)
{
    RunWithTimerMode(GetParam(), [](coop::Context* ctx)
    {
        constexpr int N = 32;
        std::atomic<int> failures{0};
        int completed = 0;

        for (int i = 0; i < N; i++)
        {
            const bool useSlack = (i % 2) == 0;
            const int64_t requestedUs = 3000 + i * 200;
            const int64_t slackUs = useSlack ? 1000 : 0;

            ctx->GetCooperator()->Spawn([&, requestedUs, slackUs](coop::Context* child)
            {
                auto t0 = std::chrono::steady_clock::now();
                auto r = coop::time::Sleep(child, std::chrono::microseconds(requestedUs),
                                           std::chrono::microseconds(slackUs));
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();

                // Never early, regardless of slack.
                //
                if (r != coop::time::SleepResult::Ok || elapsed < requestedUs)
                {
                    failures++;
                }
                // Slack overshoot bounded to roughly one slack tick (plus scheduler latency slack).
                //
                if (slackUs > 0 && elapsed > requestedUs + slackUs + 3000)
                {
                    failures++;
                }
                completed++;
            });
        }

        while (completed < N)
        {
            ctx->Yield(true);
        }
        EXPECT_EQ(failures.load(), 0);
    });
}

// A context killed mid-sleep wakes with Killed, and its deadline is cleanly removed from the queue
// by the Sleeper destructor — exercised alongside other live sleeps so the removal is an interior
// one, not just a min pop.
//
TEST_P(TimerIntegrationTest, KillMidSleepRemovesFromQueue)
{
    RunWithTimerMode(GetParam(), [](coop::Context* ctx)
    {
        coop::Context::Handle victim;
        coop::time::SleepResult victimResult = coop::time::SleepResult::Ok;
        bool victimDone = false;

        // A few long-lived background sleeps so the killed sleep is not the only node in the queue.
        //
        int bgCompleted = 0;
        for (int i = 0; i < 4; i++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* child)
            {
                coop::time::Sleep(child, 30ms);
                bgCompleted++;
            });
        }

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            victimResult = coop::time::Sleep(child, std::chrono::seconds(60));
            victimDone = true;
        }, &victim);

        // Let everything register and block.
        //
        for (int i = 0; i < 4; i++) ctx->Yield(true);

        victim.Kill();

        while (!victimDone || bgCompleted < 4)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(victimResult, coop::time::SleepResult::Killed);
        EXPECT_EQ(bgCompleted, 4);          // the kill did not disturb the other sleeps
    });
}
