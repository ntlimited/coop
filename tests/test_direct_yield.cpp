// Tests for the direct context-to-context yield fastpath (CooperatorConfiguration::directYield).
//
// The fastpath lets Context::Yield switch straight into the next runnable context instead of
// trampolining through the cooperator loop. The loop is the only place io_uring is polled, so the
// danger the fastpath must avoid is starving CQE processing -- which is what unblocks blocking IO,
// timers, and the Handle::Flash barriers that run during context teardown. The guard is a bounded
// budget: after directYieldBudget consecutive direct switches, one falls back through the loop and
// polls. These tests pin that guarantee: contexts that do nothing but spin on Yield must not be
// able to prevent a timer from firing, a blocking Recv from completing, or a killed context with
// an in-flight IO from tearing down.
//
// They run with a deliberately small budget so the fallback-and-poll boundary is crossed many
// times, and they would hang (not merely assert) if the bound were broken -- so a regression that
// removes the poll fallback surfaces as a timeout.
//

#include <atomic>
#include <chrono>
#include <functional>

#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/thread.h"

#include "coop/io/descriptor.h"
#include "coop/io/recv.h"

#include "coop/time/sleep.h"

namespace
{

// Run fn on a cooperator whose direct-yield fastpath is enabled with the given budget.
//
void RunWithDirectYield(int budget, std::function<void(coop::Context*)> fn)
{
    coop::CooperatorConfiguration cfg = coop::s_defaultCooperatorConfiguration;
    cfg.directYield = true;
    cfg.directYieldBudget = budget;

    coop::Cooperator co(cfg);
    coop::Thread t(&co);

    co.SubmitSync([&](coop::Context* ctx) { fn(ctx); });
    co.Shutdown();
}

// Contexts that spin on Yield to monopolize the runnable set, so chains of direct yields run long
// enough to repeatedly cross the budget boundary.
//
// Their state is grouped rather than passed as a bare flag pointer because the flag lives on the
// stack of the context that spawns them, and that stack is recycled the moment that context exits.
// Letting the spinners be killed at cooperator shutdown would leave them reading it afterwards; they
// have to be stopped and drained while the frame that owns the flag is still standing.
//
struct Spinners
{
    std::atomic<bool> stop{false};
    int               live{0};
};

void SpawnSpinners(coop::Context* ctx, int count, Spinners* spinners)
{
    spinners->live += count;

    for (int i = 0; i < count; ++i)
    {
        ctx->GetCooperator()->Spawn([spinners](coop::Context* c)
        {
            while (!spinners->stop.load(std::memory_order_relaxed) && !c->IsKilled())
            {
                c->Yield(true /* force */);
            }
            --spinners->live;
        });
    }
}

// Stop the spinners and hand them the CPU until every one has run its exit. Returning before that
// leaves a spinner scheduled against a dead frame.
//
void DrainSpinners(coop::Context* ctx, Spinners* spinners)
{
    spinners->stop.store(true, std::memory_order_relaxed);

    while (spinners->live > 0)
    {
        ctx->Yield(true /* force */);
    }
}

} // namespace

// Every direct-yielding context is scheduled and runs to completion -- the fastpath is a correct
// round-robin, not just a fast one. A small budget forces the loop-fallback path to interleave with
// the direct path throughout.
//
TEST(DirectYieldTest, AllContextsMakeProgress)
{
    RunWithDirectYield(4, [](coop::Context* ctx)
    {
        constexpr int kWorkers = 6;
        constexpr int kRounds = 200;

        int counts[kWorkers] = {};
        int doneCount = 0;

        for (int i = 0; i < kWorkers; ++i)
        {
            int* myCount = &counts[i];
            ctx->GetCooperator()->Spawn([myCount, &doneCount](coop::Context* c)
            {
                for (int r = 0; r < kRounds; ++r)
                {
                    ++*myCount;
                    c->Yield(true /* force */);
                }
                ++doneCount;
            });
        }

        while (doneCount < kWorkers)
        {
            ctx->Yield(true /* force */);
        }

        for (int i = 0; i < kWorkers; ++i)
        {
            EXPECT_EQ(counts[i], kRounds) << "worker " << i << " did not complete its rounds";
        }
    });
}

// A timer must fire even while other contexts spin on Yield. The sleeping context blocks (leaving
// the runnable set to the spinners); only the loop-fallback poll services the timer, so if the
// bound were broken this Sleep would never wake and the test would hang.
//
TEST(DirectYieldTest, BoundedPollServicesTimerUnderSpinners)
{
    RunWithDirectYield(4, [](coop::Context* ctx)
    {
        Spinners spinners;
        SpawnSpinners(ctx, 4, &spinners);

        // Let the spinners reach their yield loops.
        //
        for (int i = 0; i < 16; ++i) ctx->Yield(true);

        const auto t0 = std::chrono::steady_clock::now();
        coop::time::SleepResult res = coop::time::Sleep(ctx, std::chrono::milliseconds(50));
        const auto elapsed = std::chrono::steady_clock::now() - t0;

        DrainSpinners(ctx, &spinners);

        EXPECT_EQ(res, coop::time::SleepResult::Ok);

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        EXPECT_GE(ms, 40) << "slept far too short -- timer fired early?";
        EXPECT_LT(ms, 5000) << "timer starved by direct-yield spinners (poll fallback broken)";
    });
}

// A blocking Recv with no inbound data must complete (here via cancellation on kill) while spinners
// run. The kill cancels the in-flight io_uring op; the resulting CQE and the Handle teardown barrier
// are only serviced by the loop, so this also exercises the Flash-style teardown path under the
// fastpath. A broken poll bound hangs at the `while (!done)` drain.
//
TEST(DirectYieldTest, KilledInflightRecvTearsDownUnderSpinners)
{
    RunWithDirectYield(4, [](coop::Context* ctx)
    {
        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

        Spinners spinners;
        SpawnSpinners(ctx, 4, &spinners);

        auto* uring = coop::GetUring();
        coop::Coordinator ready;
        ready.TryAcquire(ctx);

        coop::Context::Handle childHandle;
        int result = 0;
        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::io::Descriptor reader(fds[0], uring);
            char buf[64] = {};
            ready.Release(child, false);
            result = coop::io::RecvKill(reader, buf, sizeof(buf));
            done = true;
        }, &childHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        childHandle.Kill();
        while (!done)
        {
            ctx->Yield(true);
        }

        DrainSpinners(ctx, &spinners);

        EXPECT_EQ(result, -ECANCELED);

        close(fds[1]);
    });
}
