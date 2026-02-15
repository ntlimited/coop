#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/signal.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"
#include "coop/time/interval.h"
#include "test_helpers.h"

// Spawn several contexts that yield in loops. Call Shutdown() and verify that the cooperator
// loop terminates (Thread joins).
//
TEST(ShutdownTest, ShutdownKillsAllContexts)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    std::atomic<int> running{0};

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        auto* running = static_cast<std::atomic<int>*>(arg);

        for (int i = 0; i < 5; i++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* child)
            {
                running->fetch_add(1, std::memory_order_relaxed);
                while (!child->IsKilled())
                {
                    child->Yield(true);
                }
            });
        }

        // All 5 children are now yielding in loops. Wait until they've all started, then
        // trigger shutdown.
        //
        while (running->load(std::memory_order_relaxed) < 5)
        {
            ctx->Yield(true);
        }

        ctx->GetCooperator()->Shutdown();
    }, &running);

    // Thread destructor joins — if Shutdown didn't work, this hangs forever.
    //
}

// Spawn contexts blocked on coordinators/signals. Shutdown() should kill them and unblock them
// so the cooperator exits cleanly.
//
TEST(ShutdownTest, ShutdownWithBlockedContexts)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        coop::Coordinator coord(ctx);
        coop::Signal sig(ctx);

        // Child blocked on coordinator acquire
        //
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coord.Acquire(child);
            // If we get here, we were unblocked
            //
            coord.Release(child, false);
        });

        // Child blocked on signal wait
        //
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            sig.Wait(child);
        });

        // Both children are now blocked. Shutdown should kill them and unblock them.
        //
        ctx->GetCooperator()->Shutdown();
    }, nullptr);
}

// Call Shutdown() multiple times on the same cooperator — should not crash.
//
TEST(ShutdownTest, ShutdownIdempotent)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        ctx->GetCooperator()->Shutdown();
        ctx->GetCooperator()->Shutdown();
        ctx->GetCooperator()->Shutdown();
    }, nullptr);
}

// Shutdown a cooperator then try Submit — should return false.
//
TEST(ShutdownTest, SubmitDuringShutdown)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        ctx->GetCooperator()->Shutdown();
    }, nullptr);

    // Thread joins here — cooperator is done. Submit after shutdown should fail.
    // We need to wait for the thread to finish first.
    //
}

// Have threads blocked in Submit (queue full), then Shutdown — they should all wake up and
// get false. We deliberately don't Launch() the cooperator so the queue never drains,
// guaranteeing the availability semaphore is exhausted and the submitter threads truly block.
//
TEST(ShutdownTest, SubmitDrainsDuringShutdown)
{
    coop::Cooperator cooperator;

    auto noop = [](coop::Context*, void*) {};

    // The availability semaphore starts at 8, but the FixedList<8> ring buffer uses one slot
    // as sentinel so the actual queue capacity is 7. Fill 7 slots so the next Submit blocks
    // on the queue being full.
    //
    for (int i = 0; i < 7; i++)
    {
        EXPECT_TRUE(cooperator.Submit(noop, nullptr));
    }

    // Now launch threads that will block in Submit because the availability semaphore is
    // exhausted (no cooperator thread is consuming submissions).
    //
    constexpr int BLOCKED_SUBMITTERS = 4;
    std::atomic<int> entered{0};
    std::atomic<int> rejected{0};
    std::vector<std::thread> blockedThreads;

    for (int i = 0; i < BLOCKED_SUBMITTERS; i++)
    {
        blockedThreads.emplace_back([&]()
        {
            entered.fetch_add(1, std::memory_order_relaxed);
            bool ok = cooperator.Submit(noop, nullptr);
            if (!ok)
            {
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Wait for the submitter threads to enter Submit
    //
    while (entered.load(std::memory_order_relaxed) < BLOCKED_SUBMITTERS)
    {
        std::this_thread::yield();
    }

    // Small delay to let them actually block on the semaphore
    //
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Shutdown triggers chain-release on the availability semaphore, waking all blocked threads
    //
    cooperator.Shutdown();

    for (auto& th : blockedThreads)
    {
        th.join();
    }

    EXPECT_EQ(rejected.load(std::memory_order_relaxed), BLOCKED_SUBMITTERS);
}

// Create 3 cooperators on separate threads, call ShutdownAll(), verify all exit cleanly.
//
TEST(ShutdownTest, ShutdownAllMultipleCooperators)
{
    constexpr int N = 3;

    coop::Cooperator cooperators[N];
    std::vector<std::unique_ptr<coop::Thread>> threads;

    std::atomic<int> running{0};

    for (int i = 0; i < N; i++)
    {
        threads.emplace_back(std::make_unique<coop::Thread>(&cooperators[i]));

        cooperators[i].Submit([](coop::Context* ctx, void* arg)
        {
            auto* running = static_cast<std::atomic<int>*>(arg);
            running->fetch_add(1, std::memory_order_relaxed);

            while (!ctx->IsKilled())
            {
                ctx->Yield(true);
            }
        }, &running);
    }

    // Wait until all cooperators have a running context
    //
    while (running.load(std::memory_order_relaxed) < N)
    {
        std::this_thread::yield();
    }

    coop::Cooperator::ShutdownAll();

    // Thread destructors join — if ShutdownAll didn't work, this hangs forever.
    //
    threads.clear();

    // Reset so subsequent tests can create cooperators normally
    //
    coop::Cooperator::ResetGlobalShutdown();
}

// Call ShutdownAll() with nothing registered — should be a no-op.
//
TEST(ShutdownTest, ShutdownAllNoCooperators)
{
    coop::Cooperator::ShutdownAll();
    coop::Cooperator::ResetGlobalShutdown();
}

// After ResetGlobalShutdown(), new cooperators should register and run normally.
//
TEST(ShutdownTest, ShutdownAllThenReset)
{
    // First round: start cooperators, shut them all down
    //
    {
        coop::Cooperator cooperator;
        coop::Thread t(&cooperator);

        cooperator.Submit([](coop::Context* ctx, void* arg)
        {
            while (!ctx->IsKilled())
            {
                ctx->Yield(true);
            }
        }, nullptr);

        coop::Cooperator::ShutdownAll();
    }

    coop::Cooperator::ResetGlobalShutdown();

    // Second round: a new cooperator should work normally
    //
    std::atomic<bool> ran{false};

    {
        coop::Cooperator cooperator;
        coop::Thread t(&cooperator);

        cooperator.Submit([](coop::Context* ctx, void* arg)
        {
            auto* ran = static_cast<std::atomic<bool>*>(arg);
            ran->store(true, std::memory_order_relaxed);
            ctx->GetCooperator()->Shutdown();
        }, &ran);
    }

    EXPECT_TRUE(ran.load(std::memory_order_relaxed));
}

TEST(SleepTest, SleepCompletesNormally)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto result = coop::time::Sleep(ctx, std::chrono::milliseconds(100));
        EXPECT_EQ(result, coop::time::SleepResult::Ok);
    });
}

TEST(SleepTest, SleepReturnsFalseOnKill)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle handle;
        coop::time::SleepResult sleepResult = coop::time::SleepResult::Ok;
        bool completed = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            sleepResult = coop::time::Sleep(child, std::chrono::seconds(60));
            completed = true;
        }, &handle);

        // Child is now blocked in Sleep. Kill it.
        //
        handle.Kill();

        // Yield until the child finishes. The killed child's Sleeper destructor cancels the
        // pending io_uring timeout, which requires the Uring context to process cancel CQEs
        // before the child can complete.
        //
        while (!completed)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(sleepResult, coop::time::SleepResult::Killed);
        EXPECT_TRUE(completed);
    });
}
