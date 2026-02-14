#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/context.h"
#include "coop/signal.h"
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
