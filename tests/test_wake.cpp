#include <gtest/gtest.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <optional>
#include "coop/continuation.h"
#include "coop/cooperator.h"

namespace
{
int Eventfds()
{
    int count = 0;
    DIR* dir = opendir("/proc/self/fd");
    if (!dir)
        return -1;
    while (auto* entry = readdir(dir))
    {
        char path[128], target[128];
        snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
        ssize_t len = readlink(path, target, sizeof(target) - 1);
        if (len < 0)
            continue;
        target[len] = 0;
        if (strstr(target, "eventfd"))
            ++count;
    }
    closedir(dir);
    return count;
}

template<typename Config>
void EnableTiered(Config& config)
{
    // Allows the resource regression to run against releases predating this option.
    if constexpr (requires { config.tieredWake; })
        config.tieredWake = true;
}
} // namespace

TEST(TieredWakeTest, OptInCreatesEventfdOnlyForFallback)
{
    int before = Eventfds();
    ASSERT_GE(before, 0);
    auto config = coop::s_defaultCooperatorConfiguration;
    EnableTiered(config);
    coop::Cooperator co(config);
    EXPECT_EQ(Eventfds(), before + (co.WakeUsesMsgRing() ? 0 : 1));
}

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "coop/detail/doorbell.h"
#include "coop/thread.h"
#include "coop/cooperate.h"
#include "coop/io/read.h"
#include "coop/io/descriptor.h"
#include "coop/time/sleep.h"
#include "coop/io/timeout.h"

TEST(DoorbellTest, NotificationBeforeColdInvalidatesSnapshot)
{
    coop::detail::Doorbell bell;
    auto expected = bell.Snapshot();
    EXPECT_EQ(bell.Notify(), coop::detail::WakeTier::Hot);
    EXPECT_FALSE(bell.Enter(expected, coop::detail::WakeTier::Cold));
    EXPECT_EQ(bell.Snapshot(), 4u);
}

TEST(DoorbellTest, ColdBeforeNotificationRequiresKernelMessage)
{
    coop::detail::Doorbell bell;
    auto expected = bell.Snapshot();
    ASSERT_TRUE(bell.Enter(expected, coop::detail::WakeTier::Cold));
    EXPECT_EQ(bell.Notify(), coop::detail::WakeTier::Cold);
    bell.Hot();
    EXPECT_EQ(bell.Snapshot(), 4u);
}

TEST(DoorbellTest, MonitorRechecksNotificationBeforeWaiting)
{
    coop::detail::Doorbell bell;
    auto expected = bell.Snapshot();
    ASSERT_TRUE(bell.Enter(expected, coop::detail::WakeTier::SemiCold));
    bool waited = false;
    coop::detail::MonitorThenWait(
        bell, 1, [&] { EXPECT_EQ(bell.Notify(), coop::detail::WakeTier::SemiCold); },
        [&] { waited = true; });
    EXPECT_FALSE(waited);
    coop::detail::MonitorThenWait(
        bell, bell.Snapshot(), [] {}, [&] { waited = true; });
    EXPECT_TRUE(waited);
}

TEST(DoorbellTest, ConcurrentColdHandshakeHasNoStoreOnlyLostWake)
{
    coop::detail::Doorbell bell;
    std::atomic<int> phase{0};
    std::atomic<bool> message{false};
    std::thread producer(
        [&]
        {
            for (int i = 1; i <= 20000; ++i)
            {
                while (phase.load() != i)
                    std::this_thread::yield();
                message.store(bell.Notify() == coop::detail::WakeTier::Cold);
                phase.store(-i);
            }
        });
    for (int i = 1; i <= 20000; ++i)
    {
        auto expected = bell.Snapshot();
        phase.store(i);
        bool parked = bell.Enter(expected, coop::detail::WakeTier::Cold);
        while (phase.load() != -i)
            std::this_thread::yield();
        if (parked)
            EXPECT_TRUE(message.load());
        bell.Hot();
    }
    producer.join();
    EXPECT_EQ(bell.Snapshot(), 80000u);
}

namespace
{
struct RunningTiered
{
    coop::Cooperator co;
    coop::Thread thread;
    explicit RunningTiered(uint32_t hot = 1, uint32_t semi = 10)
        : co(Config(hot, semi)), thread(&co)
    {
    }
    ~RunningTiered() { co.Shutdown(); }
    static coop::CooperatorConfiguration Config(uint32_t hot, uint32_t semi)
    {
        auto config = coop::s_defaultCooperatorConfiguration;
        config.tieredWake = true;
        config.wakeHotUs = hot;
        config.wakeSemiColdUs = semi;
        return config;
    }
};

bool AwaitCold(coop::Cooperator& co)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end)
    {
        if (co.WakeState() == coop::detail::WakeTier::Cold)
            return true;
        std::this_thread::yield();
    }
    return false;
}
} // namespace

TEST(TieredWakeTest, PrelaunchAndRepeatedColdSubmit)
{
    alarm(20);
    auto config = RunningTiered::Config(0, 0);
    coop::Cooperator co(config);
    int count = 0;
    ASSERT_TRUE(co.Submit([&](coop::Context*) { ++count; }));
    {
        coop::Thread thread(&co);
        for (int i = 0; i < 500; ++i)
        {
            EXPECT_TRUE(AwaitCold(co));
            EXPECT_TRUE(co.SubmitSync([&](coop::Context*) { ++count; }));
        }
        co.Shutdown();
    }
    EXPECT_EQ(count, 501);
    EXPECT_FALSE(co.SubmitSync([](coop::Context*) {}));
    alarm(0);
}

TEST(TieredWakeTest, ConcurrentProducersAndShutdown)
{
    alarm(20);
    std::atomic<int> count{0};
    {
        RunningTiered running;
        std::vector<std::thread> producers;
        for (int p = 0; p < 4; ++p)
            producers.emplace_back(
                [&]
                {
                    for (int i = 0; i < 250; ++i)
                        EXPECT_TRUE(running.co.SubmitSync([&](coop::Context*) { ++count; }));
                });
        for (auto& producer : producers)
            producer.join();
    }
    EXPECT_EQ(count.load(), 1000);
    alarm(0);
}

TEST(TieredWakeTest, CrossThreadKillAndShutdownDrainCancellation)
{
    alarm(20);
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    for (bool shutdown : {false, true})
    {
        coop::Context::Handle handle;
        std::atomic<int> result{0};
        {
            RunningTiered running(0, 0);
            ASSERT_TRUE(running.co.SubmitSync(
                [&](coop::Context*)
                {
                    running.co.Spawn(
                        [&](coop::Context* child)
                        {
                            // Keep the read alive after the submitting parent returns.
                            child->Detach();
                            coop::io::Descriptor desc(coop::io::borrowed, fds[0]);
                            char byte;
                            result.store(coop::io::ReadKill(desc, &byte, 1));
                        },
                        &handle);
                }));
            EXPECT_TRUE(AwaitCold(running.co));
            EXPECT_EQ(result.load(), 0);
            if (shutdown)
                running.co.Shutdown();
            else
                handle.Kill();
            // Observe completion before scope exit can provide a shutdown wake.
            auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (result.load() == 0 && std::chrono::steady_clock::now() < end)
                std::this_thread::yield();
            EXPECT_EQ(result.load(), -ECANCELED);
        }
        EXPECT_EQ(result.load(), -ECANCELED);
    }
    close(fds[0]);
    close(fds[1]);
    alarm(0);
}

TEST(TieredWakeTest, CooperateReplyAcrossColdOwners)
{
    alarm(20);
    RunningTiered target(0, 0);
    RunningTiered caller(0, 0);
    std::atomic<bool> ran{false};
    EXPECT_TRUE(caller.co.SubmitSync(
        [&](coop::Context* ctx)
        {
            coop::CooperateHandle handle(ctx);
            EXPECT_TRUE(target.co.Cooperate([&](coop::Context*) { ran.store(true); }, &handle));
            EXPECT_TRUE(handle.Wait(ctx));
        }));
    EXPECT_TRUE(ran.load());
    alarm(0);
}

TEST(TieredWakeTest, ShutdownCleanupCanStillParkInKernel)
{
    alarm(20);
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    std::atomic<bool> cleanup{false};
    std::atomic<int> result{0};
    {
        RunningTiered running(0, 0);
        ASSERT_TRUE(running.co.Submit(
            [&](coop::Context* ctx)
            {
                ctx->GetKilledSignal()->Wait(ctx);
                cleanup.store(true);
                coop::io::Descriptor desc(coop::io::borrowed, fds[0]);
                char byte;
                result.store(coop::io::Read(desc, &byte, 1));
            }));
        EXPECT_TRUE(AwaitCold(running.co));
        running.co.Shutdown();
        while (!cleanup.load())
            std::this_thread::yield();
        EXPECT_TRUE(AwaitCold(running.co));
        char byte = 1;
        EXPECT_EQ(write(fds[1], &byte, 1), 1);
    }
    EXPECT_EQ(result.load(), 1);
    close(fds[0]);
    close(fds[1]);
    alarm(0);
}

TEST(TieredWakeTest, ProducersRaceShutdownAndLateSubmissionsAreRejected)
{
    alarm(20);
    for (int round = 0; round < 25; ++round)
    {
        RunningTiered running(0, 0);
        std::atomic<int> attempts{0};
        std::vector<std::thread> producers;
        for (int p = 0; p < 4; ++p)
            producers.emplace_back(
                [&]
                {
                    for (int i = 0; i < 1000; ++i)
                    {
                        ++attempts;
                        if (!running.co.SubmitSync([](coop::Context*) {}))
                            break;
                    }
                });
        while (attempts.load() < 16)
            std::this_thread::yield();
        running.co.Shutdown();
        for (auto& producer : producers)
            producer.join();
        EXPECT_FALSE(running.co.SubmitSync([](coop::Context*) {}));
    }
    alarm(0);
}

// Exercise the scheduler idle boundary directly so the obsolete CQE cannot be consumed by
// an earlier outer-loop poll. The only test seam is private scheduler access; real timers,
// the real ring, and the ordinary tiered polling/blocking implementation remain in use.
namespace coop
{
struct TieredWakeTestAccess
{
    static Context* SubmissionDrainer(Cooperator& co)
    {
        Context* drainer = nullptr;
        co.m_contexts.Visit(
            [&](Context* ctx)
            {
                if (strcmp(ctx->GetName(), "SubmissionDrainer") == 0)
                    drainer = ctx;
                return true;
            });
        return drainer;
    }
    static void Arm(Cooperator& co) { co.ArmNearestTimer(); }
    static void Wait(Cooperator& co)
    {
        Context::Handle drainer;
        co.WaitTiered(false, drainer);
    }
    static void Service(Cooperator& co) { co.ServiceExpiredTimers(); }
};
} // namespace coop

TEST(TieredWakeTest, CancelledEarliestTimerRearmsBeforeCold)
{
    alarm(10);
    auto config = RunningTiered::Config(100, 0);
    config.timerMode = coop::TimerMode::UserspaceQueue;
    coop::Cooperator co(config);
    std::atomic<bool> finished{false};
    std::atomic<bool> rescued{false};
    {
        coop::Thread thread(&co);
        ASSERT_TRUE(co.Submit(
            [&](coop::Context* ctx)
            {
                co.GetUring()->Poll(); // Drain the submission wake before creating either timer.
                coop::time::Sleeper later(ctx, std::chrono::seconds(1));
                ASSERT_TRUE(later.Arm());
                {
                    coop::time::Sleeper earlier(ctx, std::chrono::milliseconds(1));
                    ASSERT_TRUE(earlier.Arm());
                    coop::TieredWakeTestAccess::Arm(co);
                    co.GetUring()->Submit(); // Submit A without dispatching its expiry.
                }
                // No sleeper is due when this obsolete CQE arrives. Make it pending immediately
                // before the idle boundary so ready() consumes it, rather than the outer loop.
                while (!co.GetUring()->HasPendingCompletions())
                    std::this_thread::yield();
                coop::TieredWakeTestAccess::Wait(co);
                EXPECT_FALSE(rescued.load());
                EXPECT_TRUE(later.Wait());
                finished.store(true);
                co.Shutdown();
            }));
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!finished.load() && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        if (!finished.load())
        {
            rescued.store(true);
            co.Shutdown(); // Bound the broken implementation's hang and join safely.
        }
    }
    EXPECT_TRUE(finished.load());
    EXPECT_FALSE(rescued.load());
    alarm(0);
}

namespace
{
bool denyMsgRingProbe = false; // Set only while constructing a test cooperator.
}

extern "C" int __real_io_uring_register_probe(io_uring*, io_uring_probe*, unsigned);
extern "C" int __wrap_io_uring_register_probe(io_uring* ring, io_uring_probe* probe, unsigned count)
{
    if (denyMsgRingProbe)
    {
        probe->ops_len = 0;
        return 0;
    }
    return __real_io_uring_register_probe(ring, probe, count);
}

TEST(TieredWakeTest, UnsupportedMsgRingUsesEventfdForColdSubmitCancelAndShutdown)
{
    alarm(20);
    int before = Eventfds();
    ASSERT_GE(before, 0);
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    {
        auto config = RunningTiered::Config(0, 0);
        config.wakeDiagnostics = true;
        denyMsgRingProbe = true;
        coop::Cooperator co(config);
        denyMsgRingProbe = false;
        EXPECT_FALSE(co.WakeUsesMsgRing());
        EXPECT_EQ(Eventfds(), before + 1);
        int count = 0;
        EXPECT_TRUE(co.Submit([&](coop::Context*) { ++count; }));
        {
            coop::Thread thread(&co);
            for (int i = 0; i < 128; ++i)
            {
                EXPECT_TRUE(AwaitCold(co));
                auto cold = co.WakeCount(coop::detail::WakeTier::Cold);
                EXPECT_TRUE(co.SubmitSync([&](coop::Context*) { ++count; }));
                EXPECT_GT(co.WakeCount(coop::detail::WakeTier::Cold), cold);
            }
            for (bool shutdown : {false, true})
            {
                coop::Context::Handle handle;
                std::atomic<int> result{0};
                EXPECT_TRUE(co.SubmitSync(
                    [&](coop::Context*)
                    {
                        co.Spawn(
                            [&](coop::Context* child)
                            {
                                child->Detach(); // Keep the read alive after its submitting parent.
                                coop::io::Descriptor desc(coop::io::borrowed, fds[0]);
                                char byte;
                                result.store(coop::io::ReadKill(desc, &byte, 1));
                            },
                            &handle);
                    }));
                EXPECT_TRUE(AwaitCold(co));
                auto cold = co.WakeCount(coop::detail::WakeTier::Cold);
                EXPECT_EQ(result.load(), 0);
                if (shutdown)
                    co.Shutdown();
                else
                    handle.Kill();
                auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (result.load() == 0 && std::chrono::steady_clock::now() < end)
                    std::this_thread::yield();
                EXPECT_EQ(result.load(), -ECANCELED);
                EXPECT_GT(co.WakeCount(coop::detail::WakeTier::Cold), cold);
            }
        }
        EXPECT_EQ(count, 129);
        EXPECT_FALSE(co.SubmitSync([](coop::Context*) {}));
    }
    EXPECT_EQ(Eventfds(), before);
    close(fds[0]);
    close(fds[1]);
    alarm(0);
}

TEST(TieredWakeDeathTest, EventfdReplyRemainsWakeableDuringShutdown)
{
    // Isolate a lost-wake hang without introducing unrelated rescue IO. Success
    // requires ordinary teardown before the process alarm.
    ASSERT_EXIT(
        {
            alarm(5);
            for (int backend : {0, 1, 2}) // Legacy eventfd, forced fallback, normal tiered.
                for (bool nativeCleanup : {false, true})
                {
                    auto config = RunningTiered::Config(0, 0);
                    config.tieredWake = backend != 0;
                    config.timerMode = coop::TimerMode::UserspaceQueue;
                    denyMsgRingProbe = backend == 1;
                    coop::Cooperator caller(config);
                    denyMsgRingProbe = false;
                    coop::Cooperator target;
                    std::atomic<bool> started{false};
                    std::atomic<bool> dispatched{false};
                    std::atomic<bool> killed{false};
                    std::atomic<bool> finished{false};
                    {
                        coop::Thread callerThread(&caller);
                        EXPECT_TRUE(caller.Submit(
                            [&](coop::Context* ctx)
                            {
                                coop::CooperateHandle handle(ctx);
                                started.store(true);
                                if (nativeCleanup)
                                {
                                    ctx->GetKilledSignal()->Wait(ctx);
                                    // Counted IO may finish before cleanup dispatches.
                                    coop::Coordinator delay;
                                    coop::io::Handle cleanup(ctx, caller.GetUring(), &delay);
                                    EXPECT_TRUE(coop::io::Timeout(
                                        cleanup, std::chrono::milliseconds(50)));
                                    EXPECT_EQ(cleanup.Wait(), -ETIME);
                                }
                                EXPECT_TRUE(target.Cooperate([](coop::Context*) {}, &handle));
                                dispatched.store(true);
                                if (!nativeCleanup)
                                    ctx->GetKilledSignal()->Wait(ctx);
                                killed.store(true);
                                EXPECT_TRUE(handle.Wait(ctx));
                                finished.store(true);
                            }));
                        while (!started.load())
                            std::this_thread::yield();
                        caller.Shutdown();
                        while (!dispatched.load() || !killed.load())
                            std::this_thread::yield();
                        // Let the killed drainer retire on the broken implementation.
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        coop::Thread targetThread(&target);
                        auto end =
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                        while (!finished.load() && std::chrono::steady_clock::now() < end)
                            std::this_thread::yield();
                        EXPECT_TRUE(finished.load());
                        target.Shutdown();
                    }
                    EXPECT_TRUE(finished.load());
                }
            std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST(TieredWakeDeathTest, PassiveTimersDoNotHoldShutdownOpen)
{
    ASSERT_EXIT(
        {
            alarm(5);
            for (int backend : {0, 1, 2})
            {
                auto config = RunningTiered::Config(0, 0);
                config.tieredWake = backend != 0;
                config.timerMode = coop::TimerMode::UserspaceQueue;
                denyMsgRingProbe = backend == 1;
                coop::Cooperator co(config);
                denyMsgRingProbe = false;
                coop::time::TimerNode timer;
                coop::Coordinator expiry;
                {
                    coop::Thread thread(&co);
                    EXPECT_TRUE(co.SubmitSync(
                        [&](coop::Context*)
                        {
                            EXPECT_TRUE(expiry.TryAcquire());
                            co.RegisterTimer(&timer, co.NowUs() + INT64_C(3600000000), &expiry);
                        }));
                    co.Shutdown();
                }
                // Metadata owners cancel their passive registrations after join.
                co.CancelTimer(&timer);
            }
            std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST(TieredWakeTest, PreparedTimerKeepsDeadlineAcrossBusyWork)
{
    alarm(10);
    auto config = RunningTiered::Config(100, 0);
    config.timerMode = coop::TimerMode::UserspaceQueue;
    coop::Cooperator co(config);
    {
        coop::Thread thread(&co);
        ASSERT_TRUE(co.Submit(
            [&](coop::Context* ctx)
            {
                auto start = std::chrono::steady_clock::now();
                coop::time::Sleeper sleeper(ctx, std::chrono::milliseconds(500));
                ASSERT_TRUE(sleeper.Arm());
                coop::TieredWakeTestAccess::Service(co);
                // Hold the prepared SQE for 300 ms of ordinary non-yielding application work.
                while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300))
                    std::this_thread::yield();
                EXPECT_TRUE(sleeper.Wait());
                auto elapsed = std::chrono::steady_clock::now() - start;
                EXPECT_GE(elapsed, std::chrono::milliseconds(500));
                EXPECT_LT(elapsed, std::chrono::milliseconds(700));
                co.Shutdown();
            }));
    }
    alarm(0);
}


namespace
{
bool AwaitKernelPark(coop::Cooperator& co)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end)
    {
        char path[128], wait[128] = {};
        snprintf(path, sizeof(path), "/proc/self/task/%d/wchan", co.Tid());
        if (FILE* file = fopen(path, "r"))
        {
            bool read = fgets(wait, sizeof(wait), file) != nullptr;
            fclose(file);
            if (read && strstr(wait, "io_cqring_wait"))
                return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void QueueCleanup(coop::Coordinator& coord, int& remaining)
{
    EXPECT_TRUE(coord.TryAcquire());
    coord.ContinueDetached(
        [&](coop::Coordinator*)
        {
            if (--remaining)
                QueueCleanup(coord, remaining);
        });
    coord.Release(nullptr, false);
}

class ParkedShutdownDeathTest : public ::testing::TestWithParam<int>
{
};

TEST_P(ParkedShutdownDeathTest, QueuedDetachedCleanup)
{
    const int backend = GetParam();
    ASSERT_EXIT(
        {
            alarm(6);
            auto config = RunningTiered::Config(0, 0);
            config.tieredWake = backend != 0;
            denyMsgRingProbe = backend == 1;
            coop::Cooperator co(config);
            denyMsgRingProbe = false;
            coop::Coordinator cleanup;
            int remaining = 64;
            std::atomic<bool> queued{false};
            std::atomic<bool> shutdownRequested{false};
            {
                coop::Thread thread(&co);
                EXPECT_TRUE(co.SubmitSync([](coop::Context*) {}));
                EXPECT_TRUE(AwaitKernelPark(co));
                EXPECT_TRUE(co.Submit(
                    [&](coop::Context*)
                    {
                        co.m_drainBudget = 1;
                        // Publish cleanup before requesting shutdown, then leave it runnable
                        // across loop boundaries. Its final drain wakes no application context.
                        QueueCleanup(cleanup, remaining);
                        queued.store(true);
                        while (!shutdownRequested.load())
                            std::this_thread::yield();
                    }));
                while (!queued.load())
                    std::this_thread::yield();
                co.Shutdown();
                shutdownRequested.store(true);
            }
            EXPECT_EQ(remaining, 0);
            std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_P(ParkedShutdownDeathTest, FinalCompletionOnlyWakesDetachedCleanup)
{
    const int backend = GetParam();
    // Both eventfd modes exercise retirement. The native MSG_RING backend has no drainer.
    if (backend == 2)
        GTEST_SKIP() << "MSG_RING has no eventfd read to retire";
    ASSERT_EXIT(
        {
            alarm(6);
            int fds[2];
            EXPECT_EQ(pipe(fds), 0);
            auto config = RunningTiered::Config(0, 0);
            config.tieredWake = backend != 0;
            denyMsgRingProbe = backend == 1;
            coop::Cooperator co(config);
            denyMsgRingProbe = false;
            coop::Coordinator completion;
            std::optional<coop::io::Handle> read;
            std::atomic<bool> killed{false};
            std::atomic<bool> finished{false};
            char byte = 0;
            {
                coop::Thread thread(&co);
                EXPECT_TRUE(co.SubmitSync(
                    [&](coop::Context*)
                    {
                        co.Spawn(
                            [&](coop::Context* ctx)
                            {
                                ctx->Detach();
                                ctx->GetKilledSignal()->Wait(ctx);
                                killed.store(true);
                            });
                        // Attribute the operation to the live drainer, which is retained while
                        // counted IO remains. No application context waits for this completion.
                        auto* owner = coop::TieredWakeTestAccess::SubmissionDrainer(co);
                        EXPECT_NE(owner, nullptr);
                        read.emplace(owner, co.GetUring(), &completion);
                        auto* sqe = co.GetUring()->GetSqe();
                        io_uring_prep_read(sqe, fds[0], &byte, 1, 0);
                        read->Submit(sqe);
                        completion.ContinueDetached(
                            [&](coop::Coordinator*)
                            {
                                EXPECT_EQ(read->Result(), 1);
                                finished.store(true);
                            });
                    }));
                EXPECT_TRUE(AwaitKernelPark(co));
                co.Shutdown();
                while (!killed.load())
                    std::this_thread::yield();
                // The cooperator must really park again during shutdown, with only the
                // native read and its own eventfd read outstanding. This write supplies the
                // final external CQE; there is no rescue Submit or second shutdown wake.
                EXPECT_TRUE(AwaitKernelPark(co));
                EXPECT_EQ(write(fds[1], "x", 1), 1);
            }
            EXPECT_TRUE(finished.load());
            EXPECT_EQ(byte, 'x');
            read.reset();
            close(fds[0]);
            close(fds[1]);
            std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0), "");
}

INSTANTIATE_TEST_SUITE_P(AllWakeBackends, ParkedShutdownDeathTest, ::testing::Values(0, 1, 2));
} // namespace
