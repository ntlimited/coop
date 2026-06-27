#include <gtest/gtest.h>

#include "coop/continuation.h"
#include "coop/coordinator.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "test_helpers.h"

using namespace coop;

// A continuation registered on a held coordinator fires (as a function call) when the
// coordinator is released, and Await() returns its result.
//
TEST(ContinuationTest, FiresOnReleaseAndAwaitReturns)
{
    test::RunInCooperator([](Context* a)
    {
        Coordinator coord;
        coord.Acquire(a);                       // models an operation in flight

        bool ran = false;
        auto c = coord.Continue([&](Coordinator*)
        {
            ran = true;
            return 42;
        });

        // Release from a sibling on the same cooperator, modelling completion.
        //
        a->GetCooperator()->Spawn([&](Context* b)
        {
            coord.Release(b);
        });

        int r = c.Await();
        EXPECT_TRUE(ran);
        EXPECT_EQ(r, 42);
    });
}

// Cancel() detaches the continuation so a later release does not fire it, and is idempotent.
//
TEST(ContinuationTest, CancelPreventsFire)
{
    test::RunInCooperator([](Context* a)
    {
        Coordinator coord;
        coord.Acquire(a);

        bool ran = false;
        {
            auto c = coord.Continue([&](Coordinator*)
            {
                ran = true;
                return 0;
            });
            EXPECT_TRUE(c.Cancel());
            EXPECT_FALSE(c.Cancel());           // idempotent
        }                                       // destructor: no-op cancel

        coord.Release(a);                       // wait list empty -> nothing fires
        EXPECT_FALSE(ran);
    });
}

// A void-returning continuation runs its side effect; Await() returns void.
//
TEST(ContinuationTest, VoidContinuation)
{
    test::RunInCooperator([](Context* a)
    {
        Coordinator coord;
        coord.Acquire(a);

        int sideEffect = 0;
        auto c = coord.Continue([&](Coordinator*)
        {
            sideEffect = 7;
        });

        a->GetCooperator()->Spawn([&](Context* b)
        {
            coord.Release(b);
        });

        c.Await();
        EXPECT_EQ(sideEffect, 7);
    });
}

// Dropping a continuation that already fired (without awaiting) is safe — the uncollected
// result is discarded.
//
TEST(ContinuationTest, FiredButNotAwaitedDrops)
{
    test::RunInCooperator([](Context* a)
    {
        Coordinator coord;
        coord.Acquire(a);

        bool ran = false;
        {
            auto c = coord.Continue([&](Coordinator*)
            {
                ran = true;
                return 99;
            });
            // Fire it inline (same context holds and releases), then drop without Await().
            //
            coord.Release(a, /*schedule=*/false);
        }
        EXPECT_TRUE(ran);
    });
}
