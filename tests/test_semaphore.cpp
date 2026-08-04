#include <gtest/gtest.h>

#include <vector>

#include "coop/cooperator.h"
#include "coop/semaphore.h"
#include "coop/self.h"

#include "test_helpers.h"

TEST(SemaphoreTest, TryAcquireAndPermitRaii)
{
    test::RunInCooperator([](coop::Context*)
    {
        coop::Semaphore sem(3);

        auto a = sem.TryAcquire(2);
        ASSERT_TRUE(static_cast<bool>(a));
        EXPECT_EQ(sem.Available(), 1u);

        auto b = sem.TryAcquire(2);
        EXPECT_FALSE(static_cast<bool>(b));

        {
            auto c = sem.TryAcquire(1);
            ASSERT_TRUE(static_cast<bool>(c));
            EXPECT_EQ(sem.Available(), 0u);
        }
        EXPECT_EQ(sem.Available(), 1u);   // c released on scope exit

        a.Release();
        EXPECT_EQ(sem.Available(), 3u);
        a.Release();                       // idempotent
        EXPECT_EQ(sem.Available(), 3u);
    });
}

TEST(SemaphoreTest, BlockingFifoAndNoBarging)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Semaphore sem(2);
        std::vector<int> order;

        auto held = sem.Acquire(ctx, 2);
        ASSERT_TRUE(static_cast<bool>(held));

        // First waiter wants 2 (more than will be free after one unit returns);
        // second wants 1. Strict FIFO means the small waiter must NOT barge past.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context* c)
        {
            auto p = sem.Acquire(c, 2);
            order.push_back(1);
        });
        ctx->GetCooperator()->Spawn([&](coop::Context* c)
        {
            auto p = sem.Acquire(c, 1);
            order.push_back(2);
        });

        for (int i = 0; i < 10; i++) ctx->Yield(true);
        EXPECT_EQ(sem.Waiters(), 2u);
        EXPECT_TRUE(order.empty());

        sem.Release(1);                   // hand back one unit — not enough for waiter 1
        held.Release();                    // wait, held already gave 2? see below
        // (held.Release() returns its 2 units; combined with the manual 1 this is +3)

        for (int i = 0; i < 20; i++) ctx->Yield(true);

        ASSERT_EQ(order.size(), 2u);
        EXPECT_EQ(order[0], 1);            // FIFO: the big waiter first
        EXPECT_EQ(order[1], 2);
    });
}

TEST(SemaphoreTest, PermitMovesIntoSpawnedChild)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Semaphore sem(1);
        bool childRan = false;

        {
            auto permit = sem.Acquire(ctx, 1);
            ASSERT_TRUE(static_cast<bool>(permit));

            // The admission travels with the child; the semaphore frees only when
            // the child (and its moved permit) finishes.
            //
            ctx->GetCooperator()->Spawn(
                [&childRan, p = std::move(permit)](coop::Context* c) mutable
            {
                c->Yield(true);
                childRan = true;
            });
        }
        EXPECT_EQ(sem.Available(), 0u);    // parent scope exit released nothing

        for (int i = 0; i < 10; i++) ctx->Yield(true);
        EXPECT_TRUE(childRan);
        EXPECT_EQ(sem.Available(), 1u);    // child's permit released on exit
    });
}

TEST(SemaphoreTest, AcquireKillTimeoutReturnsEmpty)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Semaphore sem(0);

        auto p = sem.AcquireKill(ctx, 1, std::chrono::milliseconds(50));
        EXPECT_FALSE(static_cast<bool>(p));
        EXPECT_EQ(sem.Waiters(), 0u);     // reservation withdrawn cleanly

        // The semaphore still works afterwards
        //
        sem.Release(1);
        auto q = sem.TryAcquire(1);
        EXPECT_TRUE(static_cast<bool>(q));
    });
}

TEST(SemaphoreTest, AcquireKillWokenByKill)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Semaphore sem(0);
        bool sawEmpty = false;

        coop::Context::Handle handle;
        ctx->GetCooperator()->Spawn([&](coop::Context* c)
        {
            auto p = sem.AcquireKill(c, 1);
            sawEmpty = !static_cast<bool>(p);
        }, &handle);

        for (int i = 0; i < 5; i++) ctx->Yield(true);
        EXPECT_EQ(sem.Waiters(), 1u);

        handle.Kill();
        for (int i = 0; i < 10; i++) ctx->Yield(true);

        EXPECT_TRUE(sawEmpty);
        EXPECT_EQ(sem.Waiters(), 0u);
    });
}

TEST(SemaphoreTest, SplitHandsPartOfAnAdmission)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Semaphore sem(4);
        auto p = sem.Acquire(ctx, 4);
        ASSERT_TRUE(static_cast<bool>(p));

        auto half = p.Split(2);
        EXPECT_EQ(p.Units(), 2u);
        EXPECT_EQ(half.Units(), 2u);

        half.Release();
        EXPECT_EQ(sem.Available(), 2u);
        p.Release();
        EXPECT_EQ(sem.Available(), 4u);
    });
}
