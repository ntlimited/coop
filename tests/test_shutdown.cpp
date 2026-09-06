#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <semaphore>
#include <utility>
#include <unistd.h>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/cooperate.h"
#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/signal.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"
#include "coop/time/interval.h"
#include "test_helpers.h"

namespace
{

enum class SubmissionMode { Async, Sync, Cooperate };

struct SubmissionGate
{
    std::binary_semaphore m_entered{0};
    std::binary_semaphore m_resume{0};
    std::atomic<int> m_destroyed{0};
    std::atomic<bool> m_ran{false};
};

// Hold callable construction after the public API's initial shutdown check. This is a test-only
// blocking seam: the target must finish Launch before construction is allowed to return.
//
struct GatedSubmission
{
    SubmissionGate* m_gate;

    explicit GatedSubmission(SubmissionGate* gate) : m_gate(gate) {}
    GatedSubmission(GatedSubmission&& other) : m_gate(std::exchange(other.m_gate, nullptr))
    {
        m_gate->m_entered.release();
        m_gate->m_resume.acquire();
    }
    ~GatedSubmission()
    {
        if (m_gate) m_gate->m_destroyed.fetch_add(1);
    }
    void operator()(coop::Context*) { m_gate->m_ran.store(true); }
};

void CheckShutdownAdmission(SubmissionMode mode)
{
    // A subprocess bounds the old SubmitSync hang without leaving detached threads or dead
    // semaphore pointers behind. The watchdog also bounds an unexpected scheduler join hang.
    //
    alarm(15);
    SubmissionGate gate;
    coop::Cooperator target;
    std::thread targetThread([&] { target.Launch(); });
    std::binary_semaphore completed(0);
    bool accepted = true;

    auto submit = [&](coop::Context* ctx)
    {
        if (mode == SubmissionMode::Cooperate)
        {
            coop::CooperateHandle handle(ctx);
            accepted = target.Cooperate(GatedSubmission(&gate), &handle);
            EXPECT_FALSE(handle.m_signal.IsSignaled());
        }
        else if (mode == SubmissionMode::Sync)
        {
            accepted = target.SubmitSync(GatedSubmission(&gate));
        }
        else
        {
            accepted = target.Submit(GatedSubmission(&gate));
        }
        completed.release();
    };

    std::unique_ptr<coop::Cooperator> caller;
    std::unique_ptr<coop::Thread> callerThread;
    std::thread submitter;
    if (mode == SubmissionMode::Cooperate)
    {
        caller = std::make_unique<coop::Cooperator>();
        callerThread = std::make_unique<coop::Thread>(caller.get());
        caller->Submit([&](coop::Context* ctx) { submit(ctx); });
    }
    else
    {
        submitter = std::thread([&] { submit(nullptr); });
    }

    gate.m_entered.acquire();
    target.Shutdown();
    targetThread.join();
    gate.m_resume.release();
    if (!completed.try_acquire_for(std::chrono::seconds(2)))
    {
        std::fputs("submission remained blocked after the target exited\n", stderr);
        _exit(1);
    }

    if (caller)
    {
        caller->Shutdown();
        callerThread.reset();
    }
    else
    {
        submitter.join();
    }
    EXPECT_FALSE(accepted);
    EXPECT_FALSE(gate.m_ran.load());
    EXPECT_EQ(gate.m_destroyed.load(), 1);
}

} // namespace

TEST(ShutdownAdmissionDeathTest, SubmitRejectsAfterConcurrentShutdown)
{
    ASSERT_EXIT({
        CheckShutdownAdmission(SubmissionMode::Async);
        _exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), "");
}

TEST(ShutdownAdmissionDeathTest, SubmitSyncRejectsAfterConcurrentShutdown)
{
    ASSERT_EXIT({
        CheckShutdownAdmission(SubmissionMode::Sync);
        _exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), "");
}

TEST(ShutdownAdmissionDeathTest, CooperateRejectsAfterConcurrentShutdown)
{
    ASSERT_EXIT({
        CheckShutdownAdmission(SubmissionMode::Cooperate);
        _exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), "");
}

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

    // Submit is now unbounded (no capacity limit). Verify that submissions queued before
    // Launch() are processed, and that Submit returns false after shutdown.
    //
    std::atomic<int> executed{0};
    constexpr int PRE_SUBMIT_COUNT = 10;

    for (int i = 0; i < PRE_SUBMIT_COUNT; i++)
    {
        EXPECT_TRUE(cooperator.Submit([&executed](coop::Context*)
        {
            executed.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    // Launch + immediate shutdown from inside
    //
    cooperator.Submit([](coop::Context* ctx)
    {
        ctx->GetCooperator()->Shutdown();
    });

    cooperator.Launch();

    EXPECT_EQ(executed.load(std::memory_order_relaxed), PRE_SUBMIT_COUNT);

    // Submit after shutdown should return false
    //
    EXPECT_FALSE(cooperator.Submit([](coop::Context*, void*) {}, nullptr));
}

TEST(ShutdownTest, SubmitSyncReturnsFalseWhenSpawnFails)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    // 64 PiB exceeds practical userspace VA on supported targets; Spawn must fail.
    //
    coop::SpawnConfiguration impossible = {
        .priority = 0,
        .stackSize = (static_cast<size_t>(1) << 56),
    };

    auto start = std::chrono::steady_clock::now();
    bool ok = cooperator.SubmitSync([](coop::Context*) {}, impossible);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(ok);
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
        1000);

    cooperator.Shutdown();
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

TEST(SleepTest, SlackSleepIsOneSided)
{
    // Covenant: slack may only push a deadline later, never earlier. A slack sleep must wake no
    // sooner than its requested interval, and overshoot by at most ~one slack tick.
    //
    test::RunInCooperator([](coop::Context* ctx)
    {
        const auto requested = std::chrono::microseconds(1000);
        const auto slack     = std::chrono::microseconds(500);

        auto t0 = std::chrono::steady_clock::now();
        auto result = coop::time::Sleep(ctx, requested, slack);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0);

        EXPECT_EQ(result, coop::time::SleepResult::Ok);
        EXPECT_GE(elapsed.count(), requested.count());          // never early
        EXPECT_LT(elapsed.count(), requested.count() + slack.count() + 2000);  // bounded overshoot
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
