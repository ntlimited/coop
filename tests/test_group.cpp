#include <gtest/gtest.h>

#include <atomic>
#include <vector>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/coordinate_with.h"
#include "coop/signal.h"
#include "coop/group.h"
#include "coop/self.h"

#include "test_helpers.h"

TEST(GroupTest, JoinsAllChildren)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int done = 0;
        {
            coop::Group g(ctx);
            for (int i = 0; i < 5; i++)
            {
                g.Go([&](coop::Context* c) { c->Yield(true); done++; });
            }
            // Children run eagerly (Spawn switches into them), so InFlight here is
            // whatever has not yet finished — the contract under test is that Wait
            // joins them all, not a mid-flight count.
            //
            EXPECT_TRUE(g.Wait());
        }
        EXPECT_EQ(done, 5);
    });
}

TEST(GroupTest, DestructorWaits)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int done = 0;
        {
            coop::Group g(ctx);
            g.Go([&](coop::Context* c) { c->Yield(true); c->Yield(true); done++; });
            // No explicit Wait — the destructor must join.
        }
        EXPECT_EQ(done, 1);
    });
}

TEST(GroupTest, VoidChildrenAlwaysSucceed)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Group g(ctx);
        g.Go([](coop::Context*) { /* void, no return */ });
        EXPECT_TRUE(g.Wait());
        EXPECT_FALSE(g.Failed());
    });
}

TEST(GroupTest, FailFastCancelsSiblings)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::atomic<int> completedNormally{0};
        bool siblingSawKill = false;

        coop::Group g(ctx);

        // A long-running sibling that blocks kill-aware; it must be cancelled.
        g.Go([&](coop::Context* c) -> bool
        {
            coop::Coordinator never;
            never.TryAcquire(c);
            auto r = coop::CoordinateWithKill(c, &never);
            siblingSawKill = r.Killed();
            completedNormally += r.Killed() ? 0 : 1;
            return true;
        });

        // A child that fails after a couple of yields.
        g.Go([&](coop::Context* c) -> bool
        {
            c->Yield(true);
            return false;   // failure triggers sibling cancellation
        });

        EXPECT_FALSE(g.Wait());
        EXPECT_TRUE(g.Failed());
        EXPECT_TRUE(siblingSawKill);
        EXPECT_EQ(completedNormally.load(), 0);
    });
}

TEST(GroupTest, BoundedFanOutLimitsInFlight)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int peak = 0;
        int current = 0;
        int total = 0;

        coop::Group g(ctx);
        g.SetLimit(3);

        // 8 children each occupy a slot across a couple of yields. With a limit of 3,
        // the group must never have more than 3 in flight at once.
        //
        for (int i = 0; i < 8; i++)
        {
            g.Go([&](coop::Context* c)
            {
                current++;
                if (current > peak) peak = current;
                c->Yield(true);
                c->Yield(true);
                current--;
                total++;
            });
        }
        EXPECT_TRUE(g.Wait());
        EXPECT_EQ(total, 8);
        EXPECT_LE(peak, 3);
    });
}

TEST(GroupTest, GoAfterFailureRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Group g(ctx);
        g.Go([](coop::Context*) -> bool { return false; });

        // Let the failure land
        for (int i = 0; i < 5; i++) ctx->Yield(true);

        bool accepted = g.Go([](coop::Context*) -> bool { return true; });
        EXPECT_FALSE(accepted);
        EXPECT_FALSE(g.Wait());
    });
}

TEST(GroupTest, EmptyGroupWaitsImmediately)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Group g(ctx);
        EXPECT_TRUE(g.Wait());
        EXPECT_EQ(g.InFlight(), 0u);
    });
}

// -------------------------------------------------------------------------------------
// Daemon contexts are excluded from the "real work" count
// -------------------------------------------------------------------------------------

TEST(DaemonTest, DaemonExcludedFromNonDaemonCount)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto* co = ctx->GetCooperator();
        size_t baseNonDaemon = co->NonDaemonContexts();

        coop::Signal hold(ctx);   // broadcast: one Notify wakes all waiters

        // A regular child raises the non-daemon count; a daemon child does not.
        //
        co->Spawn([&](coop::Context* c) { hold.Wait(c); });
        co->Spawn({.priority = 0, .stackSize = 16384, .daemon = true},
                  [&](coop::Context* c) { hold.Wait(c); });

        for (int i = 0; i < 5; i++) ctx->Yield(true);

        // +1 for the regular child; the daemon is excluded.
        //
        EXPECT_EQ(co->NonDaemonContexts(), baseNonDaemon + 1);

        hold.Notify(ctx, false);
        for (int i = 0; i < 10; i++) ctx->Yield(true);
        EXPECT_EQ(co->NonDaemonContexts(), baseNonDaemon);
    });
}
