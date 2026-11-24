#include <array>
#include <atomic>
#include <optional>
#include <cstdlib>
#include <new>
#include <thread>
#include <sys/syscall.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/cooperate.h"
#include "coop/cooperate_submission.h"
#include "test_helpers.h"
#include "coop/context.h"
#include "coop/context_var.h"
#include "coop/self.h"
#include "coop/signal.h"
#include "coop/thread.h"

namespace
{
std::array<std::atomic<pid_t>, 2> allocationProbeThreads{};
std::array<std::atomic<unsigned>, 2> allocationProbeCounts{};

void RecordSubmissionAllocation()
{
    for (size_t i = 0; i != allocationProbeThreads.size(); ++i)
    {
        pid_t const tracked = allocationProbeThreads[i].load(std::memory_order_relaxed);
        if (tracked != 0 && tracked == syscall(SYS_gettid))
            allocationProbeCounts[i].fetch_add(1, std::memory_order_relaxed);
    }
}

struct SubmissionAllocationProbe
{
    size_t index;
    explicit SubmissionAllocationProbe(size_t i) : index(i)
    {
        allocationProbeCounts[i].store(0);
        allocationProbeThreads[i].store(syscall(SYS_gettid));
    }
    ~SubmissionAllocationProbe() { allocationProbeThreads[index].store(0); }
    unsigned Count() const { return allocationProbeCounts[index].load(); }
};
}

extern "C" void* __real_malloc(size_t);
extern "C" void* __wrap_malloc(size_t size)
{
    RecordSubmissionAllocation();
    return __real_malloc(size);
}
extern "C" void* __real_aligned_alloc(size_t, size_t);
extern "C" void* __wrap_aligned_alloc(size_t alignment, size_t size)
{
    RecordSubmissionAllocation();
    return __real_aligned_alloc(alignment, size);
}
extern "C" int __real_posix_memalign(void**, size_t, size_t);
extern "C" int __wrap_posix_memalign(void** pointer, size_t alignment, size_t size)
{
    RecordSubmissionAllocation();
    return __real_posix_memalign(pointer, alignment, size);
}

#define COOP_WRAP_NEW(name) \
    extern "C" void* __real_##name(size_t); \
    extern "C" void* __wrap_##name(size_t size) \
    { RecordSubmissionAllocation(); return __real_##name(size); }
COOP_WRAP_NEW(_Znwm)
COOP_WRAP_NEW(_Znam)
#undef COOP_WRAP_NEW

#define COOP_WRAP_NOTHROW_NEW(name) \
    extern "C" void* __real_##name(size_t, std::nothrow_t const&) noexcept; \
    extern "C" void* __wrap_##name(size_t size, std::nothrow_t const& tag) noexcept \
    { RecordSubmissionAllocation(); return __real_##name(size, tag); }
COOP_WRAP_NOTHROW_NEW(_ZnwmRKSt9nothrow_t)
COOP_WRAP_NOTHROW_NEW(_ZnamRKSt9nothrow_t)
#undef COOP_WRAP_NOTHROW_NEW

#define COOP_WRAP_ALIGNED_NEW(name) \
    extern "C" void* __real_##name(size_t, std::align_val_t); \
    extern "C" void* __wrap_##name(size_t size, std::align_val_t alignment) \
    { RecordSubmissionAllocation(); return __real_##name(size, alignment); }
COOP_WRAP_ALIGNED_NEW(_ZnwmSt11align_val_t)
COOP_WRAP_ALIGNED_NEW(_ZnamSt11align_val_t)
#undef COOP_WRAP_ALIGNED_NEW

#define COOP_WRAP_ALIGNED_NOTHROW_NEW(name) \
    extern "C" void* __real_##name(size_t, std::align_val_t, std::nothrow_t const&) noexcept; \
    extern "C" void* __wrap_##name(size_t size, std::align_val_t alignment, \
                                 std::nothrow_t const& tag) noexcept \
    { RecordSubmissionAllocation(); return __real_##name(size, alignment, tag); }
COOP_WRAP_ALIGNED_NOTHROW_NEW(_ZnwmSt11align_val_tRKSt9nothrow_t)
COOP_WRAP_ALIGNED_NOTHROW_NEW(_ZnamSt11align_val_tRKSt9nothrow_t)
#undef COOP_WRAP_ALIGNED_NOTHROW_NEW

namespace coop
{
// Test-only access isolates the real completion path from stack allocation and unrelated
// scheduler work, and selects the final-drain branch without a wall-clock shutdown race.
struct CooperateSubmissionTestAccess
{
    static unsigned Borrowers(CooperateSubmission& submission)
    {
        std::lock_guard<std::mutex> lock(submission.m_lock);
        return submission.m_borrowers;
    }
    template<typename State>
    static void ObserveCancellation(CooperateSubmission& submission)
    {
        submission.m_cancellation.m_invoke = [](SubmissionEntry* entry, Context*)
        {
            auto* owner = static_cast<CooperateSubmission::Cancellation*>(entry)->owner;
            static_cast<State*>(owner->m_state)->inNotification = true;
            owner->DeliverCancellation();
        };
    }
    static bool Waiting(CooperateSubmission& submission)
    { return submission.m_waiting.load(std::memory_order_acquire); }
    static void DrainRemaining(Cooperator& target) { target.DrainRemainingSubmissions(); }
    struct CompletionProbe : SubmissionEntry
    {
        CooperateSubmission* submission;
        unsigned allocations{99};
        CountdownLatch finished{1};
        explicit CompletionProbe(CooperateSubmission* slot) : submission(slot)
        {
            m_inline = true;
            m_invoke = [](SubmissionEntry* entry, Context*)
            {
                auto* probe = static_cast<CompletionProbe*>(entry);
                {
                    SubmissionAllocationProbe allocations(1);
                    probe->submission->Release();
                    probe->allocations = allocations.Count();
                }
                probe->finished.Arrive();
            };
            m_destroy = [](SubmissionEntry*) {};
        }
    };
    static void HoldCompletion(CooperateSubmission& submission)
    {
        std::lock_guard<std::mutex> lock(submission.m_lock);
        ++submission.m_borrowers;
    }
    static void QueueCompletionProbe(Cooperator& target, CompletionProbe& probe)
    { target.PushSubmission(&probe, true); }
};
}

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
        EXPECT_TRUE(handle.m_completion.IsReady());
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
        EXPECT_FALSE(handle.m_completion.IsReady());

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

namespace
{
struct OwnedWork
{
    std::atomic<unsigned> bodies{0};
    std::atomic<unsigned> completions{0};
    coop::CountdownLatch started{1};
    coop::CountdownLatch childCleanup{1};
    coop::CountdownLatch releaseChild{1};
    std::atomic<bool> childFinished{false};

    static void Body(coop::Context*, void* state)
    {
        ++static_cast<OwnedWork*>(state)->bodies;
    }

    static void Complete(void* state, coop::CooperateSubmission::Status)
    {
        ++static_cast<OwnedWork*>(state)->completions;
    }

    static void WithChild(coop::Context* ctx, void* state)
    {
        auto* work = static_cast<OwnedWork*>(state);
        ++work->bodies;
        EXPECT_TRUE(ctx->GetCooperator()->Spawn([work](coop::Context* child)
        {
            child->GetKilledSignal()->Wait(child);
            work->childCleanup.Arrive();
            work->releaseChild.Wait(child);
            work->childFinished.store(true, std::memory_order_release);
        }));
        work->started.Arrive();
        ctx->GetKilledSignal()->Wait(ctx);
    }
};
} // namespace

namespace
{
struct OwnedContextCleanup
{
    bool* finished{nullptr};
    void (*cleanup)(void*){nullptr};
    void* state{nullptr};
    ~OwnedContextCleanup()
    {
        if (finished) *finished = true;
        if (cleanup) cleanup(state);
    }
};
coop::ContextVar<OwnedContextCleanup> ownedContextCleanup;
}

TEST(CooperateTest, OwnedQueuedCancellationNeverInvokesBody)
{
    coop::Cooperator target;
    OwnedWork work;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::CooperateSubmission submission(OwnedWork::Body, &work, OwnedWork::Complete);
        ASSERT_TRUE(target.Cooperate(submission));
        submission.Cancel();
        {
            coop::Thread targetThread(&target);
            EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Cancelled);
            target.Shutdown();
        }
        EXPECT_EQ(work.bodies.load(), 0u);
        EXPECT_EQ(work.completions.load(), 1u);
    });
}

TEST(CooperateTest, OwnedRunningCancellationJoinsEveryAttachedBorrower)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    OwnedWork work;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::CooperateSubmission submission(OwnedWork::WithChild, &work, OwnedWork::Complete);
        ASSERT_TRUE(target.Cooperate(submission));
        work.started.Wait(ctx);
        EXPECT_FALSE(target.Cooperate(submission));
        EXPECT_EQ(submission.GetStatus(), coop::CooperateSubmission::Status::Running);
        bool returned = false;
        coop::CountdownLatch joined(1);
        ASSERT_TRUE(ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            unsigned allocations;
            {
                SubmissionAllocationProbe probe(0);
                submission.Cancel();
                allocations = probe.Count();
            }
            EXPECT_EQ(allocations, 0u);
            EXPECT_EQ(submission.CancelAndWait(child),
                      coop::CooperateSubmission::Status::Killed);
            returned = true;
            joined.Arrive();
        }));
        work.childCleanup.Wait(ctx);
        EXPECT_FALSE(returned);
        EXPECT_FALSE(work.childFinished.load(std::memory_order_acquire));
        work.releaseChild.Arrive();
        joined.Wait(ctx);
        EXPECT_TRUE(work.childFinished.load(std::memory_order_acquire));
        EXPECT_EQ(work.bodies.load(), 1u);
        EXPECT_EQ(work.completions.load(), 1u);
    });
    target.Shutdown();
}

TEST(CooperateTest, OwnedClosedTargetRefusesAndCompletesExactlyOnce)
{
    coop::Cooperator target;
    target.Shutdown();
    OwnedWork work;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::CooperateSubmission submission(OwnedWork::Body, &work, OwnedWork::Complete);
        EXPECT_FALSE(target.Cooperate(submission));
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Refused);
        submission.Cancel();
        EXPECT_EQ(work.bodies.load(), 0u);
        EXPECT_EQ(work.completions.load(), 1u);
    });
}

TEST(CooperateTest, OwnedShutdownReturnsEveryQueuedSubmission)
{
    coop::Cooperator target;
    OwnedWork work;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        constexpr size_t count = 64;
        std::array<std::optional<coop::CooperateSubmission>, count> submissions;
        for (auto& submission : submissions)
        {
            submission.emplace(OwnedWork::Body, &work, OwnedWork::Complete);
            ASSERT_TRUE(target.Cooperate(*submission));
        }
        target.Shutdown();
        {
            coop::Thread targetThread(&target);
            for (auto& submission : submissions)
                EXPECT_EQ(submission->Wait(ctx), coop::CooperateSubmission::Status::Refused);
        }
        EXPECT_EQ(work.bodies.load(), 0u);
        EXPECT_EQ(work.completions.load(), count);
    }, {.priority = 0, .stackSize = 65536});
}

TEST(CooperateTest, OwnedKilledWaitCancelsAndJoinsWork)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    OwnedWork work;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::CooperateSubmission submission([](coop::Context* worker, void* state)
        {
            auto* work = static_cast<OwnedWork*>(state);
            ++work->bodies;
            work->started.Arrive();
            worker->GetKilledSignal()->Wait(worker);
        }, &work, OwnedWork::Complete);
        ASSERT_TRUE(target.Cooperate(submission));
        work.started.Wait(ctx);
        ctx->KillSelf(coop::KillCause::Kill);
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Killed);
        EXPECT_EQ(work.bodies.load(), 1u);
        EXPECT_EQ(work.completions.load(), 1u);
    });
    target.Shutdown();
}

TEST(CooperateTest, OwnedCancellationRaceCompletesOnce)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    test::RunInCooperator([&](coop::Context* ctx)
    {
        for (unsigned round = 0; round != 128; ++round)
        {
            OwnedWork work;
            coop::CooperateSubmission submission(OwnedWork::Body, &work, OwnedWork::Complete);
            ASSERT_TRUE(target.Cooperate(submission));
            std::thread cancel([&] { submission.Cancel(); });
            auto const status = submission.Wait(ctx);
            cancel.join();
            EXPECT_TRUE(status == coop::CooperateSubmission::Status::Completed ||
                        status == coop::CooperateSubmission::Status::Cancelled ||
                        status == coop::CooperateSubmission::Status::Killed);
            EXPECT_LE(work.bodies.load(), 1u);
            EXPECT_EQ(work.completions.load(), 1u);
        }
    });
    target.Shutdown();
}

TEST(CooperateTest, OwnedSubmissionAndCompletionDoNotAllocate)
{
    coop::Cooperator target;
    struct State
    {
        coop::CooperateSubmissionTestAccess::CompletionProbe* probe{nullptr};
        unsigned completions{0};
    } state;
    coop::CooperateSubmission submission([](coop::Context*, void* borrowed)
    {
        ownedContextCleanup->state = borrowed;
        ownedContextCleanup->cleanup = [](void* borrowed)
        {
            auto* state = static_cast<State*>(borrowed);
            coop::CooperateSubmissionTestAccess::QueueCompletionProbe(
                *coop::Self()->GetCooperator(), *state->probe);
        };
    }, &state, [](void* borrowed, coop::CooperateSubmission::Status)
    { ++static_cast<State*>(borrowed)->completions; });
    // Retain one extra notification borrower, exactly as running cancellation does.
    // Its inline entry is queued during ContextVar teardown, after which the worker
    // retires before the entry can run. The probe scopes only the real final Release,
    // callback and latch arrival on the target owner; scheduler/logging/stack work is outside.
    coop::CooperateSubmissionTestAccess::CompletionProbe completionProbe(&submission);
    state.probe = &completionProbe;
    coop::CooperateSubmissionTestAccess::HoldCompletion(submission);
    unsigned submitAllocations;
    bool accepted;
    {
        SubmissionAllocationProbe probe(0);
        accepted = target.Cooperate(submission);
        submitAllocations = probe.Count();
    }
    EXPECT_TRUE(accepted);
    EXPECT_EQ(submitAllocations, 0u);
    {
        coop::Thread targetThread(&target);
        test::RunInCooperator([&](coop::Context* ctx)
        {
            EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Completed);
            completionProbe.finished.Wait(ctx);
            EXPECT_EQ(completionProbe.allocations, 0u);
        });
        target.Shutdown();
    }
    EXPECT_EQ(state.completions, 1u);
}

TEST(CooperateTest, OwnedAllocationWitnessCoversEveryAllocatorShape)
{
    auto witness = [](auto allocate)
    {
        unsigned allocations;
        {
            SubmissionAllocationProbe probe(0);
            allocate();
            allocations = probe.Count();
        }
        EXPECT_GE(allocations, 1u);
    };
    witness([] { void* (*volatile allocate)(size_t) = std::malloc;
                 auto* p = allocate(64); std::free(p); });
    witness([] { void* (*volatile allocate)(size_t, size_t) = std::aligned_alloc;
                 auto* p = allocate(64, 64); std::free(p); });
    witness([] { void* p; EXPECT_EQ(posix_memalign(&p, 64, 64), 0); std::free(p); });
    witness([] { auto* p = ::operator new(64); ::operator delete(p); });
    witness([] { auto* p = ::operator new[](64); ::operator delete[](p); });
    witness([] { auto* p = ::operator new(64, std::nothrow); ::operator delete(p); });
    witness([] { auto* p = ::operator new[](64, std::nothrow); ::operator delete[](p); });
    witness([] { auto* p = ::operator new(64, std::align_val_t(64));
                 ::operator delete(p, std::align_val_t(64)); });
    witness([] { auto* p = ::operator new[](64, std::align_val_t(64));
                 ::operator delete[](p, std::align_val_t(64)); });
    witness([] { auto* p = ::operator new(64, std::align_val_t(64), std::nothrow);
                 ::operator delete(p, std::align_val_t(64)); });
    witness([] { auto* p = ::operator new[](64, std::align_val_t(64), std::nothrow);
                 ::operator delete[](p, std::align_val_t(64)); });
}

TEST(CooperateTest, OwnedBodyReturnCancelsAndJoinsAttachedChildren)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    OwnedWork work;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::CooperateSubmission submission([](coop::Context* worker, void* state)
        {
            auto* work = static_cast<OwnedWork*>(state);
            EXPECT_TRUE(worker->GetCooperator()->Spawn([work](coop::Context* child)
            {
                child->GetKilledSignal()->Wait(child);
                work->childCleanup.Arrive();
                work->releaseChild.Wait(child);
                work->childFinished.store(true, std::memory_order_release);
            }));
            work->started.Arrive();
        }, &work, OwnedWork::Complete);
        ASSERT_TRUE(target.Cooperate(submission));
        work.started.Wait(ctx);
        work.childCleanup.Wait(ctx);
        EXPECT_EQ(work.completions.load(), 0u);
        submission.Cancel(); // Body outcome is frozen even while attached children retire.
        work.releaseChild.Arrive();
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Completed);
        EXPECT_TRUE(work.childFinished.load(std::memory_order_acquire));
        EXPECT_EQ(work.completions.load(), 1u);
    });
    target.Shutdown();
}

TEST(CooperateTest, OwnedShutdownAdmissionRaceRetiresEverySlot)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    OwnedWork work;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        constexpr size_t count = 64;
        std::array<std::optional<coop::CooperateSubmission>, count> submissions;
        std::binary_semaphore startShutdown(0);
        std::thread shutdown([&]
        {
            startShutdown.acquire();
            target.Shutdown();
        });
        for (size_t i = 0; i != count; ++i)
        {
            submissions[i].emplace(OwnedWork::Body, &work, OwnedWork::Complete);
            target.Cooperate(*submissions[i]);
            if (i == 8)
                startShutdown.release();
        }
        shutdown.join();
        for (auto& submission : submissions)
        {
            auto const status = submission->Wait(ctx);
            EXPECT_TRUE(status == coop::CooperateSubmission::Status::Completed ||
                        status == coop::CooperateSubmission::Status::Cancelled ||
                        status == coop::CooperateSubmission::Status::Killed ||
                        status == coop::CooperateSubmission::Status::Refused);
        }
        EXPECT_LE(work.bodies.load(), count);
        EXPECT_EQ(work.completions.load(), count);
    }, {.priority = 0, .stackSize = 65536});
}

TEST(CooperateTest, OwnedDeadlineWaitCancelsAndJoinsWork)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    OwnedWork work;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::CooperateSubmission submission([](coop::Context* worker, void* state)
        {
            auto* work = static_cast<OwnedWork*>(state);
            work->started.Arrive();
            worker->GetKilledSignal()->Wait(worker);
        }, &work, OwnedWork::Complete);
        ASSERT_TRUE(target.Cooperate(submission));
        work.started.Wait(ctx);
        ctx->SetDeadline(1);
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Killed);
        EXPECT_EQ(ctx->WhyKilled(), coop::KillCause::Deadline);
        EXPECT_EQ(work.completions.load(), 1u);
    });
    target.Shutdown();
}

TEST(CooperateTest, OwnedSelfSubmissionJoinsCompletion)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        OwnedWork work;
        coop::CooperateSubmission submission(OwnedWork::Body, &work, OwnedWork::Complete);
        ASSERT_TRUE(ctx->GetCooperator()->Cooperate(submission));
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Completed);
        EXPECT_EQ(work.bodies.load(), 1u);
        EXPECT_EQ(work.completions.load(), 1u);
    });
}

TEST(CooperateTest, OwnedCompletionFollowsContextVariableRetirement)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    test::RunInCooperator([&](coop::Context* ctx)
    {
        struct State
        {
            bool cleaned{false};
            bool completeAfterCleanup{false};
            bool completedOnScheduler{false};
        } state;
        coop::CooperateSubmission submission([](coop::Context*, void* borrowed)
        {
            ownedContextCleanup->finished = &static_cast<State*>(borrowed)->cleaned;
        }, &state, [](void* borrowed, coop::CooperateSubmission::Status)
        {
            auto* state = static_cast<State*>(borrowed);
            state->completeAfterCleanup = state->cleaned;
            state->completedOnScheduler = coop::Self() == nullptr;
        });
        ASSERT_TRUE(target.Cooperate(submission));
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Completed);
        EXPECT_TRUE(state.completeAfterCleanup);
        EXPECT_TRUE(state.completedOnScheduler);
    });
    target.Shutdown();
}

TEST(CooperateTest, OwnedUnlaunchedTargetDestructionReturnsSubmission)
{
    OwnedWork work;
    coop::CooperateSubmission submission(OwnedWork::Body, &work, OwnedWork::Complete);
    {
        coop::Cooperator target;
        ASSERT_TRUE(target.Cooperate(submission));
    }
    EXPECT_EQ(work.bodies.load(), 0u);
    EXPECT_EQ(work.completions.load(), 1u);
    test::RunInCooperator([&](coop::Context* ctx)
    {
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Refused);
    });
}

TEST(CooperateTest, OwnedGlobalRegistrationRefusalReturnsSubmission)
{
    OwnedWork work;
    coop::CooperateSubmission submission(OwnedWork::Body, &work, OwnedWork::Complete);
    coop::Cooperator target;
    ASSERT_TRUE(target.Cooperate(submission));
    coop::Cooperator::ShutdownAll();
    target.Launch();
    coop::Cooperator::ResetGlobalShutdown();
    EXPECT_EQ(work.bodies.load(), 0u);
    EXPECT_EQ(work.completions.load(), 1u);
    test::RunInCooperator([&](coop::Context* ctx)
    {
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Refused);
    });
}

TEST(CooperateTest, OwnedLateCancellationPreservesReturnedBodyOutcome)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    struct State
    {
        coop::CountdownLatch cleanupRegistered{1};
        coop::CountdownLatch releaseCleanup{1};
    } state;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::CooperateSubmission submission([](coop::Context*, void* borrowed)
        {
            ownedContextCleanup->state = borrowed;
            ownedContextCleanup->cleanup = [](void* borrowed)
            {
                auto* state = static_cast<State*>(borrowed);
                state->releaseCleanup.WaitKill(coop::Self(), nullptr, state, [](void* borrowed)
                { static_cast<State*>(borrowed)->cleanupRegistered.Arrive(); });
            };
        }, &state);
        ASSERT_TRUE(target.Cooperate(submission));
        state.cleanupRegistered.Wait(ctx);
        // The body has returned and its ContextVar destructor registered a real wait.
        submission.Cancel();
        state.releaseCleanup.Arrive();
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Completed);
    });
    target.Shutdown();
}

TEST(CooperateTest, OwnedCancellationNotificationCanBeLastBorrower)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    struct State
    {
        coop::CooperateSubmission* submission{nullptr};
        bool sawTwoBorrowers{false};
        bool completedInNotification{false};
        bool inNotification{false};
        unsigned completions{0};
    } state;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::CooperateSubmission submission([](coop::Context*, void* borrowed)
        {
            ownedContextCleanup->state = borrowed;
            ownedContextCleanup->cleanup = [](void* borrowed)
            {
                auto* state = static_cast<State*>(borrowed);
                state->submission->Cancel();
                state->sawTwoBorrowers =
                    coop::CooperateSubmissionTestAccess::Borrowers(*state->submission) == 2;
                // No yield: the context retires and drops its borrower before this queued
                // cancellation can run. Its notification must release the final borrower.
            };
        }, &state, [](void* borrowed, coop::CooperateSubmission::Status result)
        {
            auto* state = static_cast<State*>(borrowed);
            state->completedInNotification = state->inNotification;
            ++state->completions;
            EXPECT_EQ(result, coop::CooperateSubmission::Status::Completed);
        });
        state.submission = &submission;
        coop::CooperateSubmissionTestAccess::ObserveCancellation<State>(submission);
        ASSERT_TRUE(target.Cooperate(submission));
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Completed);
        EXPECT_TRUE(state.sawTwoBorrowers);
        EXPECT_TRUE(state.completedInNotification);
        EXPECT_EQ(state.completions, 1u);
    });
    target.Shutdown();
}

TEST(CooperateTest, OwnedRunningCancellationSurvivesRefusedNotification)
{
    coop::Cooperator target;
    struct State
    {
        coop::CooperateSubmission* submission{nullptr};
        unsigned allocations{99};
        unsigned borrowers{0};
        unsigned completions{0};
    } state;
    coop::CooperateSubmission submission([](coop::Context* ctx, void* borrowed)
    {
        auto* state = static_cast<State*>(borrowed);
        ctx->GetCooperator()->Shutdown();
        {
            SubmissionAllocationProbe probe(1);
            state->submission->Cancel();
            state->allocations = probe.Count();
        }
        state->borrowers = coop::CooperateSubmissionTestAccess::Borrowers(*state->submission);
        // The cancel notification was refused after admission closed. The scheduler's
        // shutdown sweep must supply the kill, without retaining a phantom borrower.
        ctx->GetKilledSignal()->Wait(ctx);
    }, &state, [](void* borrowed, coop::CooperateSubmission::Status)
    { ++static_cast<State*>(borrowed)->completions; });
    state.submission = &submission;
    ASSERT_TRUE(target.Cooperate(submission));
    {
        coop::Thread targetThread(&target);
        test::RunInCooperator([&](coop::Context* ctx)
        { EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Killed); });
    }
    EXPECT_EQ(state.allocations, 0u);
    EXPECT_EQ(state.borrowers, 1u);
    EXPECT_EQ(state.completions, 1u);
}

TEST(CooperateTest, OwnedFinalDrainConsumesQueuedCancellationExactlyOnce)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    struct State
    {
        coop::CooperateSubmission* submission{nullptr};
        unsigned beforeDrain{0};
        unsigned afterDrain{0};
        unsigned completions{0};
    } state;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::CooperateSubmission submission([](coop::Context*, void* borrowed)
        {
            ownedContextCleanup->state = borrowed;
            ownedContextCleanup->cleanup = [](void* borrowed)
            {
                auto* state = static_cast<State*>(borrowed);
                state->submission->Cancel();
                state->beforeDrain =
                    coop::CooperateSubmissionTestAccess::Borrowers(*state->submission);
                // Force the real final-drain branch on the owner with a real queued
                // cancellation node; ordinary scheduler pickup cannot run before this.
                auto* target = coop::Self()->GetCooperator();
                coop::CooperateSubmissionTestAccess::DrainRemaining(*target);
                state->afterDrain =
                    coop::CooperateSubmissionTestAccess::Borrowers(*state->submission);
            };
        }, &state, [](void* borrowed, coop::CooperateSubmission::Status)
        { ++static_cast<State*>(borrowed)->completions; });
        state.submission = &submission;
        ASSERT_TRUE(target.Cooperate(submission));
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Completed);
        EXPECT_EQ(state.beforeDrain, 2u);
        EXPECT_EQ(state.afterDrain, 1u);
        EXPECT_EQ(state.completions, 1u);
    });
    target.Shutdown();
}

TEST(CooperateTest, OwnedPlainThreadSuccessorFreesOnlyAfterRetirement)
{
    coop::Cooperator target;
    coop::Thread targetThread(&target);
    struct Operation
    {
        std::atomic<bool> callbackPublished{false};
        bool effect{false};
        coop::CooperateSubmission submission{
            [](coop::Context*, void* borrowed)
            { static_cast<Operation*>(borrowed)->effect = true; }, this,
            [](void* borrowed, coop::CooperateSubmission::Status result)
            {
                auto* op = static_cast<Operation*>(borrowed);
                EXPECT_EQ(result, coop::CooperateSubmission::Status::Completed);
                EXPECT_FALSE(op->submission.IsRetired());
                op->callbackPublished.store(true, std::memory_order_release);
            }};
    };
    auto* op = new Operation;
    std::atomic<bool> submitReturned{false};
    // The successor is already alive when the callback publishes. It intentionally never
    // uses a cooperative Context or Wait, and must not treat callback delivery as retirement.
    std::thread successor([op, &submitReturned]
    {
        while (!op->callbackPublished.load(std::memory_order_acquire))
            std::this_thread::yield();
        while (!op->submission.IsRetired())
            std::this_thread::yield();
        while (!submitReturned.load(std::memory_order_acquire))
            std::this_thread::yield();
        EXPECT_TRUE(op->effect);
        delete op;
    });
    // Join publication as well as library retirement before the successor frees storage.
    EXPECT_TRUE(target.Cooperate(op->submission));
    submitReturned.store(true, std::memory_order_release);
    successor.join();
    target.Shutdown();
}

TEST(CooperateTest, OwnedIdleWaitAndRepeatedSubmissionAreRefusedSafely)
{
    coop::Cooperator target;
    OwnedWork work;
    coop::CooperateSubmission submission(OwnedWork::Body, &work, OwnedWork::Complete);
    test::RunInCooperator([&](coop::Context* ctx)
    {
        EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Idle);
        EXPECT_TRUE(submission.IsRetired());
        EXPECT_TRUE(target.Cooperate(submission));
        EXPECT_FALSE(target.Cooperate(submission));
        EXPECT_EQ(submission.GetStatus(), coop::CooperateSubmission::Status::Queued);
        {
            coop::Thread targetThread(&target);
            EXPECT_EQ(submission.Wait(ctx), coop::CooperateSubmission::Status::Completed);
            EXPECT_FALSE(target.Cooperate(submission));
            EXPECT_EQ(submission.GetStatus(), coop::CooperateSubmission::Status::Completed);
            EXPECT_TRUE(submission.IsRetired());
            target.Shutdown();
        }
    });
    EXPECT_EQ(work.bodies.load(), 1u);
    EXPECT_EQ(work.completions.load(), 1u);
}

TEST(CooperateDeathTest, OwnedQueuedDestructionTerminatesInEveryBuild)
{
    EXPECT_DEATH({
        coop::Cooperator target;
        auto* submission = new coop::CooperateSubmission([](coop::Context*, void*) {}, nullptr);
        target.Cooperate(*submission);
        delete submission;
        std::_Exit(0);
    }, "destroyed before retirement");
}

TEST(CooperateDeathTest, OwnedRunningDestructionTerminatesInEveryBuild)
{
    EXPECT_DEATH({
        coop::Cooperator target;
        std::binary_semaphore entered(0);
        auto* submission = new coop::CooperateSubmission([](coop::Context* ctx, void* state)
        {
            static_cast<std::binary_semaphore*>(state)->release();
            ctx->GetKilledSignal()->Wait(ctx);
        }, &entered);
        target.Cooperate(*submission);
        coop::Thread targetThread(&target);
        entered.acquire();
        delete submission;
        std::_Exit(0);
    }, "destroyed before retirement");
}

TEST(CooperateDeathTest, OwnedConcurrentWaitersTerminateInEveryBuild)
{
    EXPECT_DEATH({
        coop::Cooperator target;
        coop::CooperateSubmission submission([](coop::Context*, void*) {}, nullptr);
        target.Cooperate(submission);
        test::RunInCooperator([&](coop::Context* ctx)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* first)
            { submission.Wait(first); });
            // Spawn enters the child synchronously; it can return control only after
            // publishing its waiter and suspending on the still-unlaunched target.
            EXPECT_TRUE(coop::CooperateSubmissionTestAccess::Waiting(submission));
            EXPECT_FALSE(submission.IsRetired());
            submission.Wait(ctx);
        });
    }, "permits one cooperative waiter");
}
