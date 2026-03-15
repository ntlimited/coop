#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/cooperate.h"
#include "coop/context.h"
#include "coop/self.h"
#include "coop/signal.h"
#include "coop/thread.h"

TEST(CooperateTest, BasicCooperate)
{
    // Two cooperators on separate threads. Cooperate dispatches work from one to the other
    // and the handle confirms spawn success.
    //
    coop::Cooperator target;
    coop::Thread targetThread(&target);

    std::atomic<bool> workRan{false};

    {
        coop::Cooperator caller;
        coop::Thread callerThread(&caller);

        caller.Submit([&](coop::Context* ctx)
        {
            coop::CooperateHandle handle(ctx);

            bool enqueued = target.Cooperate([&workRan](coop::Context*)
            {
                workRan.store(true, std::memory_order_release);
            }, &handle);
            EXPECT_TRUE(enqueued);

            bool spawned = handle.Wait(ctx);
            EXPECT_TRUE(spawned);

            // Give the work time to complete (it runs on target cooperator).
            //
            auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(2000);
            while (!workRan.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < deadline)
            {
                ctx->Yield(true);
            }
            EXPECT_TRUE(workRan.load(std::memory_order_acquire));

            caller.Shutdown();
        });
    }
    // callerThread destructor joined above

    target.Shutdown();
}

TEST(CooperateTest, CooperateFireAndForget)
{
    // Cooperate with handle = nullptr — work should still execute.
    //
    coop::Cooperator target;
    coop::Thread targetThread(&target);

    std::atomic<bool> workRan{false};

    {
        coop::Cooperator caller;
        coop::Thread callerThread(&caller);

        caller.Submit([&](coop::Context* ctx)
        {
            bool enqueued = target.Cooperate([&workRan](coop::Context*)
            {
                workRan.store(true, std::memory_order_release);
            });
            EXPECT_TRUE(enqueued);

            // Give the work time to complete.
            //
            auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(2000);
            while (!workRan.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < deadline)
            {
                ctx->Yield(true);
            }
            EXPECT_TRUE(workRan.load(std::memory_order_acquire));

            caller.Shutdown();
        });
    }

    target.Shutdown();
}

TEST(CooperateTest, CooperateSelfFastPath)
{
    // Cooperate to own cooperator — inline spawn + immediate handle signal.
    //
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    cooperator.Submit([&](coop::Context* ctx)
    {
        bool workRan = false;
        coop::CooperateHandle handle(ctx);

        bool result = ctx->GetCooperator()->Cooperate([&workRan](coop::Context*)
        {
            workRan = true;
        }, &handle);

        EXPECT_TRUE(result);

        // Self-cooperator fast path: handle is signaled synchronously.
        //
        EXPECT_TRUE(handle.m_signal.IsSignaled());
        EXPECT_TRUE(handle.m_spawnOk);
        EXPECT_TRUE(workRan);

        cooperator.Shutdown();
    });
}

TEST(CooperateTest, CooperateTargetShutdown)
{
    // Target is shutting down — Cooperate returns false, handle is NOT signaled.
    //
    coop::Cooperator target;
    {
        coop::Thread targetThread(&target);
        target.Shutdown();
    }

    coop::Cooperator caller;
    coop::Thread callerThread(&caller);

    caller.Submit([&](coop::Context* ctx)
    {
        coop::CooperateHandle handle(ctx);

        bool enqueued = target.Cooperate([](coop::Context*) {}, &handle);
        EXPECT_FALSE(enqueued);

        // Handle should NOT have been signaled since we returned false immediately.
        //
        EXPECT_FALSE(handle.m_signal.IsSignaled());

        caller.Shutdown();
    });
}

TEST(CooperateTest, CooperateSpawnBeforeWorkCompletes)
{
    // The handle signals spawn success, not work completion. Verify by holding the work
    // open with a gate that the caller controls — the handle fires while work is blocked.
    //
    coop::Cooperator target;
    coop::Thread targetThread(&target);

    std::atomic<bool> workStarted{false};
    std::atomic<bool> workDone{false};
    std::atomic<bool> releaseGate{false};

    {
        coop::Cooperator caller;
        coop::Thread callerThread(&caller);

        caller.Submit([&](coop::Context* ctx)
        {
            coop::CooperateHandle handle(ctx);

            bool enqueued = target.Cooperate(
                [&workStarted, &workDone, &releaseGate](coop::Context* wCtx)
                {
                    workStarted.store(true, std::memory_order_release);

                    // Spin-yield until the caller releases us. This ensures the work is
                    // still running when the handle fires.
                    //
                    while (!releaseGate.load(std::memory_order_acquire))
                        wCtx->Yield(true);

                    workDone.store(true, std::memory_order_release);
                }, &handle);
            EXPECT_TRUE(enqueued);

            bool spawned = handle.Wait(ctx);
            EXPECT_TRUE(spawned);

            // The work should have started (spawned) but not completed (gate is closed).
            //
            // Wait for the work to confirm it started.
            //
            auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(2000);
            while (!workStarted.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < deadline)
            {
                ctx->Yield(true);
            }
            EXPECT_TRUE(workStarted.load(std::memory_order_acquire));
            EXPECT_FALSE(workDone.load(std::memory_order_acquire));

            // Release the gate so the work can complete.
            //
            releaseGate.store(true, std::memory_order_release);

            deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(2000);
            while (!workDone.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < deadline)
            {
                ctx->Yield(true);
            }
            EXPECT_TRUE(workDone.load(std::memory_order_acquire));

            caller.Shutdown();
        });
    }

    target.Shutdown();
}
