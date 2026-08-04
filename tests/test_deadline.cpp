#include <gtest/gtest.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/coordinate_with.h"
#include "coop/self.h"

#include "test_helpers.h"

TEST(DeadlineTest, TightenOnlyAndInheritance)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        EXPECT_EQ(ctx->DeadlineUs(), 0);
        EXPECT_EQ(ctx->RemainingUs(), INT64_MAX);
        EXPECT_EQ(ctx->WhyKilled(), coop::KillCause::None);

        ctx->SetDeadlineIn(std::chrono::seconds(10));
        int64_t d = ctx->DeadlineUs();
        EXPECT_GT(d, 0);

        // Setting a looser deadline does not extend
        ctx->SetDeadlineIn(std::chrono::seconds(100));
        EXPECT_EQ(ctx->DeadlineUs(), d);

        // A tighter one wins
        ctx->SetDeadlineIn(std::chrono::seconds(5));
        EXPECT_LT(ctx->DeadlineUs(), d);
        int64_t tight = ctx->DeadlineUs();

        // Children inherit at spawn
        int64_t childDeadline = -1;
        ctx->GetCooperator()->Spawn([&](coop::Context* c)
        {
            childDeadline = c->DeadlineUs();
        });
        for (int i = 0; i < 5; i++) ctx->Yield(true);
        EXPECT_EQ(childDeadline, tight);
    });
}

TEST(DeadlineTest, ExpiredWaitKillsWithCause)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // A holder child owns the coordinator and parks forever (until killed). The
        // main context sets a deadline and blocks on that coordinator — a real block,
        // so the cooperator polls io_uring and the deadline timeout fires cleanly
        // (no parent busy-yield starving the CQE).
        //
        coop::Coordinator coord;
        coop::Context::Handle holder;
        ctx->GetCooperator()->Spawn([&](coop::Context* c)
        {
            coord.TryAcquire(c);
            coop::Signal park(c);
            coop::CoordinateWithKill(c, &park);   // parks until killed
        }, &holder);

        // Let the holder acquire the coordinator
        ctx->Yield(true);

        ctx->SetDeadlineIn(std::chrono::milliseconds(40));
        auto result = coop::CoordinateWithKill(ctx, &coord);
        EXPECT_TRUE(result.Killed());
        EXPECT_EQ(ctx->WhyKilled(), coop::KillCause::Deadline);

        holder.Kill();
    });
}

TEST(DeadlineTest, CallerTimeoutNotDoubleApplied)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // A context with a long deadline plus a short per-call timeout: the call's own
        // timeout must still fire as TimedOut (not be swallowed into a deadline kill).
        //
        ctx->SetDeadlineIn(std::chrono::seconds(10));
        coop::Coordinator coord;
        coord.TryAcquire(ctx);
        auto result = coop::CoordinateWithKill(ctx, &coord,
                                               std::chrono::milliseconds(30));
        EXPECT_TRUE(result.TimedOut());
        EXPECT_FALSE(ctx->IsKilled());
    });
}

TEST(DeadlineTest, OnKillHookFires)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        static int fired;
        fired = 0;

        coop::Context::Handle handle;
        ctx->GetCooperator()->Spawn([&](coop::Context* c)
        {
            coop::Context::KillHook hook;
            hook.fn = [](void* p) { (*static_cast<int*>(p))++; };
            hook.arg = &fired;
            c->OnKill(&hook);

            // Block until killed
            coop::Coordinator coord;
            coord.TryAcquire(c);
            coop::CoordinateWithKill(c, &coord);

            c->RemoveKillHook(&hook);   // no-op after firing (already popped)
        }, &handle);

        for (int i = 0; i < 5; i++) ctx->Yield(true);
        handle.Kill();
        for (int i = 0; i < 10; i++) ctx->Yield(true);

        EXPECT_EQ(fired, 1);
    });
}

TEST(DeadlineTest, KillCauseRecordedOnHandleKill)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::KillCause seen = coop::KillCause::None;
        bool done = false;

        coop::Context::Handle handle;
        ctx->GetCooperator()->Spawn([&](coop::Context* c)
        {
            coop::Coordinator coord;
            coord.TryAcquire(c);
            coop::CoordinateWithKill(c, &coord);
            seen = c->WhyKilled();
            done = true;
        }, &handle);

        for (int i = 0; i < 5; i++) ctx->Yield(true);
        handle.Kill(coop::KillCause::Drain);
        for (int i = 0; i < 10 && !done; i++) ctx->Yield(true);

        EXPECT_TRUE(done);
        EXPECT_EQ(seen, coop::KillCause::Drain);
    });
}
