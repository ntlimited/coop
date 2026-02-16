#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/signal.h"
#include "coop/time/interval.h"
#include "test_helpers.h"

TEST(SignalTest, StartsUnsignaled)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Signal sig(ctx);
        EXPECT_FALSE(sig.IsSignaled());
    });
}

TEST(SignalTest, NotifySignals)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Signal sig(ctx);
        sig.Notify(ctx);
        EXPECT_TRUE(sig.IsSignaled());
    });
}

TEST(SignalTest, WaitAfterNotify)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Signal sig(ctx);
        sig.Notify(ctx);

        // Wait on an already-signaled signal returns immediately
        //
        sig.Wait(ctx);
        EXPECT_TRUE(sig.IsSignaled());
    });
}

TEST(SignalTest, WaitBlocks)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Signal sig(ctx);
        int step = 0;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            EXPECT_EQ(step, 0);
            step = 1;

            // This should block until the parent notifies
            //
            sig.Wait(child);

            EXPECT_EQ(step, 2);
            step = 3;
        });

        // Child blocked on Wait, control returned here
        //
        EXPECT_EQ(step, 1);
        step = 2;
        sig.Notify(ctx);

        EXPECT_EQ(step, 3);
    });
}

TEST(SignalTest, KillSignalWorks)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle handle;
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            EXPECT_FALSE(child->IsKilled());

            // Yield back so parent can kill us
            //
            child->Yield(true);

            // Resumed — should now be killed
            //
            EXPECT_TRUE(child->IsKilled());
        }, &handle);

        // Child yielded, kill it
        //
        handle.Kill();

        // Yield to let child run its killed check
        //
        ctx->Yield(true);
    });
}

TEST(SignalTest, WaitOnKillSignal)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle handle;
        bool waited = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            // Block until killed
            //
            child->GetKilledSignal()->Wait(child);
            waited = true;
        }, &handle);

        // Child is blocked on its kill signal
        //
        EXPECT_FALSE(waited);
        handle.Kill();

        EXPECT_TRUE(waited);
    });
}

TEST(SignalTest, CoordinateWithReturnsKilled)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle handle;
        bool completed = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::Coordinator coord(child);

            // Yield so the parent can kill us
            //
            child->Yield(true);

            auto result = coop::CoordinateWith(child, &coord);
            EXPECT_TRUE(result.Killed());
            EXPECT_EQ(result.coordinator, nullptr);

            coord.Release(child, false);
            completed = true;
        }, &handle);

        handle.Kill();
        ctx->Yield(true);
        EXPECT_TRUE(completed);
    });
}

TEST(SignalTest, CoordinateWithReturnsCoordinator)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Coordinator coord(ctx);

        // Coordinator is held by ctx, release it so CoordinateWith can acquire it
        //
        coord.Release(ctx, false);

        auto result = coop::CoordinateWith(ctx, &coord);
        EXPECT_FALSE(result.Killed());
        EXPECT_TRUE(result == coord);

        coord.Release(ctx, false);
    });
}

TEST(SignalTest, CoordinateWithRepeated)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle handle;
        bool completed = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::Coordinator coord;
            coord.TryAcquire(child);

            // Yield so the parent can kill us before we call CoordinateWith
            //
            child->Yield(true);
            EXPECT_TRUE(child->IsKilled());

            // First CoordinateWith — signal already fired, so TryAcquire succeeds on the
            // signal's coordinator and re-arms m_heldBy.
            //
            auto r1 = coop::CoordinateWith(child, &coord);
            EXPECT_TRUE(child->IsKilled());

            // Second CoordinateWith — without proper cleanup, TryAcquire would fail on
            // the re-armed coordinator and deadlock here.
            //
            auto r2 = coop::CoordinateWith(child, &coord);
            EXPECT_TRUE(child->IsKilled());

            coord.Release(child, false);
            completed = true;
        }, &handle);

        // Kill the child, then yield to let it resume and run both CoordinateWith calls
        //
        handle.Kill();
        ctx->Yield(true);

        EXPECT_TRUE(completed);
    });
}

TEST(SignalTest, CoordinateWithTimesOut)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Coordinator held by ctx — will never be released, so the timeout should fire
        //
        coop::Coordinator coord(ctx);

        auto result = coop::CoordinateWith(ctx, &coord, std::chrono::milliseconds(50));
        EXPECT_TRUE(result.TimedOut());
        EXPECT_FALSE(result.Killed());
        EXPECT_EQ(result.coordinator, nullptr);

        coord.Release(ctx, false);
    });
}

TEST(SignalTest, CoordinateWithCompletesBeforeTimeout)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Coordinator coord(ctx);

        // Spawn a child that releases the coordinator before the timeout fires
        //
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coord.Release(child, false);
        });

        auto result = coop::CoordinateWith(ctx, &coord, std::chrono::milliseconds(5000));
        EXPECT_FALSE(result.TimedOut());
        EXPECT_FALSE(result.Killed());
        EXPECT_TRUE(result == coord);

        coord.Release(ctx, false);
    });
}
