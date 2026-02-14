#include <gtest/gtest.h>

#include "coop/coordinator.h"
#include "coop/self.h"
#include "test_helpers.h"

TEST(CoordinatorTest, StartsUnheld)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Coordinator coord;
        EXPECT_FALSE(coord.IsHeld());
    });
}

TEST(CoordinatorTest, TryAcquireSucceeds)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Coordinator coord;
        EXPECT_TRUE(coord.TryAcquire(ctx));
        EXPECT_TRUE(coord.IsHeld());
        coord.Release(ctx, false /* schedule */);
    });
}

TEST(CoordinatorTest, TryAcquireFails)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Coordinator coord;
        coord.TryAcquire(ctx);

        // Spawn a child that tries to acquire the already-held coordinator
        //
        bool childFailed = false;
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            childFailed = !coord.TryAcquire(child);
        });

        EXPECT_TRUE(childFailed);
        coord.Release(ctx, false);
    });
}

TEST(CoordinatorTest, AcquireBlocks)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Coordinator coord;
        coord.TryAcquire(ctx);

        int order = 0;

        // Spawn a child that will block on acquire until we release
        //
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            EXPECT_EQ(order, 0);
            order = 1;
            coord.Acquire(child);
            // Resumed after parent releases
            //
            EXPECT_EQ(order, 2);
            order = 3;
            coord.Release(child, false);
        });

        // Child ran and blocked on acquire, control returned here
        //
        EXPECT_EQ(order, 1);
        order = 2;
        coord.Release(ctx, true /* schedule: switch to unblocked child */);

        EXPECT_EQ(order, 3);
    });
}

TEST(CoordinatorTest, Flash)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Coordinator coord;
        coord.TryAcquire(ctx);

        // Flash from another context — acquire + release
        //
        bool flashed = false;
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coord.Flash(child);
            flashed = true;
        });

        // Child blocked on Flash (acquire), release from here
        //
        coord.Release(ctx, true);

        EXPECT_TRUE(flashed);
    });
}
