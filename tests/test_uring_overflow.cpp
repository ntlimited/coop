#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <tuple>

#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/io/handle.h"
#include "coop/self.h"
#include "coop/thread.h"

namespace
{

enum class RingMode
{
    Bare,
    CoopTaskrun,
    DeferTaskrun,
    SqPoll
};
enum class YieldMode
{
    Scheduler,
    Direct,
    GovernedDirect
};

class UringOverflowTest : public testing::TestWithParam<std::tuple<RingMode, YieldMode>>
{
protected:
    void OnRing(auto body, bool requireNoDrop = false)
    {
        auto const [ringMode, yieldMode] = GetParam();
        coop::CooperatorConfiguration config;
        config.uring.entries = 8;
        config.uring.coopTaskrun = ringMode == RingMode::CoopTaskrun;
        config.uring.deferTaskrun = ringMode == RingMode::DeferTaskrun;
        config.uring.sqpoll = ringMode == RingMode::SqPoll;
        config.directYield = yieldMode != YieldMode::Scheduler;
        config.directYieldBudget = yieldMode == YieldMode::GovernedDirect ? 1 << 20 : 16;
        config.ioPresentLimit = yieldMode == YieldMode::GovernedDirect ? 4 : 0;
        bool supported = true;
        coop::Cooperator co(config);
        coop::Thread thread(&co);
        EXPECT_TRUE(co.SubmitSync(
            [&](coop::Context*)
            {
                // The submission notification must be consumed and the reader re-armed;
                // a fixed yield count cannot establish that on an asynchronous SQPOLL ring.
                bool settled = false;
                auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!settled && std::chrono::steady_clock::now() < deadline)
                {
                    co.GetUring()->Poll();
                    coop::Yield();
                    co.VisitContexts(
                        [&](coop::Context* context)
                        {
                            if (co.IsWakeContext(context))
                            {
                                auto const& stats = context->m_statistics;
                                settled = stats.ioCompletes > 0 &&
                                          stats.ioSubmits == stats.ioCompletes + 1;
                            }
                            return true;
                        });
                    settled = settled && !co.GetUring()->HasPendingSubmissions() &&
                              io_uring_sq_ready(co.GetUring()->Ring()) == 0 &&
                              !co.GetUring()->HasPendingCompletions();
                    if (!settled)
                        std::this_thread::yield();
                }
                EXPECT_TRUE(settled) << "runtime wake reader did not settle before calibration";
                unsigned const flags = co.GetUring()->Ring()->flags;
                unsigned const mask =
                    IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SQPOLL;
                unsigned expected = 0;
                if (ringMode == RingMode::CoopTaskrun)
                    expected = IORING_SETUP_COOP_TASKRUN;
                if (ringMode == RingMode::DeferTaskrun)
                    expected = IORING_SETUP_DEFER_TASKRUN;
                if (ringMode == RingMode::SqPoll)
                    expected = IORING_SETUP_SQPOLL;
                supported =
                    (flags & mask) == expected &&
                    (!requireNoDrop || (co.GetUring()->Ring()->features & IORING_FEAT_NODROP));
                if (supported && settled)
                    body(co.GetUring());
                co.Shutdown();
            }));
        if (!supported)
            GTEST_SKIP() << "requested ring mode or no-drop completion support unavailable";
    }
};

struct Operation
{
    coop::Coordinator completion;
    coop::io::Handle handle;
    explicit Operation(coop::io::Uring* ring) : handle(coop::Self(), ring, &completion) {}
};

// Stop and join even when a fatal assertion returns from the test body.
struct YieldingSibling
{
    bool stop = false;
    bool exited = false;
    bool spawned = false;

    YieldingSibling()
    {
        spawned = coop::Spawn(
            [this](coop::Context*)
            {
                while (!stop)
                    coop::Yield();
                exited = true;
            });
        EXPECT_TRUE(spawned);
    }

    ~YieldingSibling()
    {
        stop = true;
        for (unsigned i = 0; spawned && !exited && i < 4096; ++i)
            coop::Yield();
        if (spawned && !exited)
            std::abort();
    }
};

TEST_P(UringOverflowTest, BusyYieldsHarvestOverflowWithoutNewSubmissions)
{
    OnRing(
        [&](coop::io::Uring* ring)
        {
            YieldingSibling sibling;
            ASSERT_TRUE(sibling.spawned);

            std::deque<Operation> operations;
            auto* raw = ring->Ring();
            ASSERT_EQ(raw->sq.ring_entries, 8u);
            ASSERT_EQ(raw->cq.ring_entries, 16u);
            int const baseline = ring->PendingOps();
            for (unsigned i = 0; i < 24; ++i)
            {
                operations.emplace_back(ring);
                auto* sqe = ring->GetSqe();
                auto const sqDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!sqe && (raw->flags & IORING_SETUP_SQPOLL) &&
                       std::chrono::steady_clock::now() < sqDeadline)
                {
                    std::this_thread::yield();
                    sqe = ring->GetSqe();
                }
                ASSERT_NE(sqe, nullptr);
                io_uring_prep_nop(sqe);
                operations.back().handle.Submit(sqe);
                ASSERT_GE(ring->Submit(), 0);
                // DEFER requires explicit task-work service during calibration as well.
                if (raw->flags & IORING_SETUP_DEFER_TASKRUN)
                    ASSERT_GE(io_uring_get_events(raw), 0);
            }

            // SQPOLL consumes asynchronously. Observe actual overflow before testing progress;
            // elapsed yields are never a substitute for this precondition.
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while ((io_uring_sq_ready(raw) != 0 ||
                    !(IO_URING_READ_ONCE(*raw->sq.kflags) & IORING_SQ_CQ_OVERFLOW)) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            EXPECT_EQ(io_uring_sq_ready(raw), 0u);
            EXPECT_NE(IO_URING_READ_ONCE(*raw->sq.kflags) & IORING_SQ_CQ_OVERFLOW, 0u);
            EXPECT_EQ(IO_URING_READ_ONCE(*raw->sq.kflags) & IORING_SQ_TASKRUN, 0u);
            EXPECT_FALSE(ring->HasPendingSubmissions());
            EXPECT_EQ(ring->ReapOnly(), 16);
            EXPECT_EQ(ring->PendingOps(), baseline + 8);
            if (!(raw->flags & IORING_SETUP_SQPOLL))
            {
                EXPECT_EQ(io_uring_cq_ready(raw), 0u);
                EXPECT_TRUE(ring->HasPendingCompletions());
            }

            // Both contexts remain runnable: no idle wait, timer or new SQE can repair progress.
            for (unsigned i = 0; i < 4096; ++i)
                coop::Yield();
            if (ring->PendingOps() != baseline)
                std::fprintf(
                    stderr,
                    "overflow progress flags=%#x sq_flags=%#x pending=%d baseline=%d cq=%u sq=%u\n",
                    raw->flags, IO_URING_READ_ONCE(*raw->sq.kflags), ring->PendingOps(), baseline,
                    io_uring_cq_ready(raw), io_uring_sq_ready(raw));
            EXPECT_EQ(ring->PendingOps(), baseline);
            EXPECT_EQ(IO_URING_READ_ONCE(*raw->sq.kflags) & IORING_SQ_CQ_OVERFLOW, 0u);

            // Repair a failed observation before borrowed handles/sibling state leave scope.
            ASSERT_GE(io_uring_get_events(raw), 0);
            ring->Poll();
            for (auto& op : operations)
                EXPECT_EQ(op.handle.Wait(), 0);
        },
        true);
}

TEST_P(UringOverflowTest, QuietPollsDoNotRequestAdditionalEnters)
{
    OnRing(
        [](coop::io::Uring* ring)
        {
            ASSERT_FALSE(ring->HasPendingSubmissions());
            ASSERT_FALSE(ring->HasPendingCompletions());
#if COOP_PERF_MODE == 1
            auto& counters = coop::GetCooperator()->GetPerfCounters();
            auto const before = counters.Get(coop::perf::Counter::PollSubmit);
#endif
            for (unsigned i = 0; i < 4096; ++i)
                EXPECT_EQ(ring->Poll(), 0);
#if COOP_PERF_MODE == 1
            EXPECT_EQ(counters.Get(coop::perf::Counter::PollSubmit), before);
#endif
            // PollSubmit guards io_uring_submit; DEFER retains its existing get_events per Poll.
            EXPECT_FALSE(ring->HasPendingCompletions());
        });
}

INSTANTIATE_TEST_SUITE_P(RingAndYieldModes, UringOverflowTest,
                         testing::Combine(testing::Values(RingMode::Bare, RingMode::CoopTaskrun,
                                                          RingMode::DeferTaskrun, RingMode::SqPoll),
                                          testing::Values(YieldMode::Scheduler, YieldMode::Direct,
                                                          YieldMode::GovernedDirect)));

} // namespace
