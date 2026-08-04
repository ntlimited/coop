#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"

// Virtual-time cooperator: a large sleep completes in negligible WALL time because the
// cooperator jumps its clock to the deadline instead of really sleeping.
//
TEST(VirtualTimeTest, LongSleepCompletesInstantly)
{
    coop::CooperatorConfiguration cfg;
    cfg.virtualTime = true;

    coop::Cooperator cooperator(cfg);
    coop::Thread thread(&cooperator);

    auto wallStart = std::chrono::steady_clock::now();
    int64_t virtualStart = 0, virtualEnd = 0;

    cooperator.SubmitSync([&](coop::Context* ctx)
    {
        auto* co = ctx->GetCooperator();
        virtualStart = co->VirtualNowUs();

        // Ten seconds of virtual sleep — would be forever in a real test.
        //
        coop::time::Sleep(ctx, std::chrono::seconds(10));

        virtualEnd = co->VirtualNowUs();
        co->Shutdown();
    });

    auto wallElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - wallStart).count();

    // Virtual clock advanced ~10s; wall clock barely moved.
    //
    EXPECT_GE(virtualEnd - virtualStart, 10'000'000);   // >= 10s in us
    EXPECT_LT(wallElapsed, 2000);                         // real time nowhere near 10s
}

// Concurrent virtual sleeps fire in deadline order, and the clock advances monotonically
// through each deadline.
//
TEST(VirtualTimeTest, ConcurrentSleepsOrderedByDeadline)
{
    coop::CooperatorConfiguration cfg;
    cfg.virtualTime = true;

    coop::Cooperator cooperator(cfg);
    coop::Thread thread(&cooperator);

    std::vector<int> order;

    cooperator.SubmitSync([&](coop::Context* ctx)
    {
        auto* co = ctx->GetCooperator();

        // Spawn sleepers with staggered deadlines; they must wake in ascending order.
        //
        for (int i : {3, 1, 5, 2, 4})
        {
            co->Spawn([&order, i](coop::Context* c)
            {
                coop::time::Sleep(c, std::chrono::seconds(i));
                order.push_back(i);
            });
        }

        // Sleep past all of them, then check.
        //
        coop::time::Sleep(ctx, std::chrono::seconds(10));
        co->Shutdown();
    });

    ASSERT_EQ(order.size(), 5u);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4, 5}));
}

// A sanity check that a non-virtual cooperator still sleeps in real time (the seam is
// off by default and unchanged).
//
TEST(VirtualTimeTest, RealTimeStillSleepsByDefault)
{
    coop::CooperatorConfiguration cfg;   // virtualTime defaults false

    coop::Cooperator cooperator(cfg);
    coop::Thread thread(&cooperator);

    auto wallStart = std::chrono::steady_clock::now();
    cooperator.SubmitSync([&](coop::Context* ctx)
    {
        coop::time::Sleep(ctx, std::chrono::milliseconds(60));
        ctx->GetCooperator()->Shutdown();
    });
    auto wallElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - wallStart).count();

    EXPECT_GE(wallElapsed, 55);   // really slept ~60ms
}
