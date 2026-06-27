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

// Firing is deferred to the cooperator loop's drain, so a continuation dropped while still
// pending (Release enqueued it, but the frame exits before the loop drains it) is cancelled, not
// fired. The wait-list node is node-based, so the dtor safely unlinks it from the pending queue.
//
TEST(ContinuationTest, DropWhilePendingCancels)
{
    test::RunInCooperator([](Context* a)
    {
        Coordinator coord;
        coord.Acquire(a);

        bool ran = false;
        {
            auto c = coord.Continue([&](Coordinator*) { ran = true; return 0; });
            coord.Release(a, /*schedule=*/false);   // enqueues; does not fire synchronously
        }                                           // dropped while pending -> cancelled

        a->Yield(true);                             // let the loop drain -- nothing should fire
        EXPECT_FALSE(ran);
    });
}

// A coordinator may carry both a continuation and a blocked context. Each Release services the
// head in FIFO order; a fired continuation leaves the coordinator unheld (no ownership handoff),
// so the context behind it is serviced on a subsequent acquire/release.
//
TEST(ContinuationTest, MixedContextAndContinuationWaiters)
{
    test::RunInCooperator([](Context* a)
    {
        Coordinator coord;
        coord.Acquire(a);                                   // a holds it

        bool contRan = false;
        auto c = coord.Continue([&](Coordinator*) { contRan = true; return 0; });

        bool ctxWoke = false;
        a->GetCooperator()->Spawn([&](Context* b)
        {
            coord.Acquire(b);                               // blocks behind the continuation
            ctxWoke = true;
        });
        a->Yield(true);                                     // let b run and block on coord

        coord.Release(a, /*schedule=*/false);               // fires continuation (head)
        a->Yield(true);                                     // drain -> continuation runs
        EXPECT_TRUE(contRan);
        EXPECT_FALSE(ctxWoke);                              // b still blocked: no handoff

        coord.Acquire(a);                                   // coord was left unheld
        coord.Release(a, /*schedule=*/true);                // now wake b
        EXPECT_TRUE(ctxWoke);
    });
}
