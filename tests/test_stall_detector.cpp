#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <csignal>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <sys/syscall.h>
#include <unistd.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/detail/stall_capture.h"
#include "coop/self.h"
#include "coop/stall_detector.h"
#include "coop/thread.h"
#include "coop/time/now.h"
#include "coop/time/sleep.h"

// A context that busy-loops without yielding pins the cooperator thread. The watchdog must catch it,
// name it, and capture a non-empty stack of where it was stuck.
//
TEST(StallDetectorTest, DetectsBusyLoopStall)
{
    coop::Cooperator co;
    coop::Thread thread(&co);

    std::atomic<int> reports{0};
    std::atomic<int> maxDepth{0};
    std::mutex nameMu;
    char capturedName[64] = {};

    coop::StallDetector det(&co, std::chrono::milliseconds(30),
        [&](const coop::StallDetector::Report& r)
        {
            reports.fetch_add(1);
            if (r.depth > maxDepth.load()) maxDepth.store(r.depth);
            std::lock_guard<std::mutex> l(nameMu);
            std::strncpy(capturedName, r.name, sizeof(capturedName) - 1);
        });

    co.SubmitSync([&](coop::Context* ctx)
    {
        ctx->SetName("staller");

        // Hog the thread for ~150ms with no yield. The watchdog's 30ms threshold should trip.
        //
        int64_t start = coop::time::MonotonicMicros();
        volatile uint64_t sink = 0;
        while (coop::time::MonotonicMicros() - start < 150000)
        {
            for (int i = 0; i < 4096; ++i) sink += i;
        }

        ctx->GetCooperator()->Shutdown();
    });

    EXPECT_GE(reports.load(), 1);
    EXPECT_GE(maxDepth.load(), 1);   // at least the stalled PC was captured
    {
        std::lock_guard<std::mutex> l(nameMu);
        EXPECT_STREQ(capturedName, "staller");
    }
}

// A context that yields cooperatively, even in a tight loop for far longer than the threshold, is
// making progress every switch -- the detector must stay silent.
//
TEST(StallDetectorTest, NoStallForCooperativeLoop)
{
    coop::Cooperator co;
    coop::Thread thread(&co);

    std::atomic<int> reports{0};

    coop::StallDetector det(&co, std::chrono::milliseconds(20),
        [&](const coop::StallDetector::Report&) { reports.fetch_add(1); });

    co.SubmitSync([&](coop::Context* ctx)
    {
        int64_t start = coop::time::MonotonicMicros();
        while (coop::time::MonotonicMicros() - start < 150000)
        {
            ctx->Yield(true);   // hand control back every iteration
        }
        ctx->GetCooperator()->Shutdown();
    });

    EXPECT_EQ(reports.load(), 0);
}

// A context that blocks (sleeps) is not running -- the cooperator is idle. Idle waiting must never
// be mistaken for a stall.
//
TEST(StallDetectorTest, NoStallWhenBlocked)
{
    coop::Cooperator co;
    coop::Thread thread(&co);

    std::atomic<int> reports{0};

    coop::StallDetector det(&co, std::chrono::milliseconds(20),
        [&](const coop::StallDetector::Report&) { reports.fetch_add(1); });

    co.SubmitSync([&](coop::Context* ctx)
    {
        for (int i = 0; i < 10; ++i)
        {
            coop::time::Sleep(ctx, std::chrono::milliseconds(15));
        }
        ctx->GetCooperator()->Shutdown();
    });

    EXPECT_EQ(reports.load(), 0);
}

// Exercise delayed delivery without an io_uring runtime. The same mailbox and queued-signal
// transport are used by the production watchdog; the native handler deliberately pauses to model
// a target descheduled after claiming its capture.
//
namespace
{

bool WaitFor(const std::atomic<int>& value, int minimum)
{
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (value.load(std::memory_order_acquire) < minimum)
    {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

coop::detail::StallCapture nativeCapture;
std::atomic<int> nativeDelivered{0};
std::atomic<int> nativeClaimed{0};
std::atomic<bool> nativePause{false};
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

void NativeCaptureHandler(int, siginfo_t* info, void*)
{
    nativeDelivered.fetch_add(1, std::memory_order_relaxed);
    if (!nativeCapture.Claim(info)) return;
    nativeClaimed.fetch_add(1, std::memory_order_release);
    while (nativePause.load(std::memory_order_acquire)) {}
    nativeCapture.depth = static_cast<int>(syscall(SYS_gettid));
    nativeCapture.Complete(reinterpret_cast<uintptr_t>(info->si_value.sival_ptr));
}

class NativeCaptureSignal
{
public:
    NativeCaptureSignal()
    {
        nativeDelivered.store(0);
        nativeClaimed.store(0);
        nativePause.store(false);
        struct sigaction action{};
        action.sa_sigaction = NativeCaptureHandler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        EXPECT_EQ(sigaction(SIGURG, &action, &m_previous), 0);
    }

    ~NativeCaptureSignal() { sigaction(SIGURG, &m_previous, nullptr); }

private:
    struct sigaction m_previous{};
};

// SIGURG is blocked before publishing the TID. Unblocking synchronously delivers the queued
// signal; joining the thread then guarantees no test handler is still using the mailbox.
//
class DelayedSignalThread
{
public:
    DelayedSignalThread() : m_thread([this]
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGURG);
        pthread_sigmask(SIG_BLOCK, &set, nullptr);
        tid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    }) {}

    ~DelayedSignalThread() { Join(); }

    void Join()
    {
        release.store(true, std::memory_order_release);
        if (m_thread.joinable()) m_thread.join();
    }

    std::atomic<int> tid{0};
    std::atomic<bool> release{false};

private:
    std::thread m_thread;
};

} // namespace

TEST(StallCaptureTest, LateSignalCannotClaimAnotherThreadsRequest)
{
    NativeCaptureSignal handler;
    DelayedSignalThread first, second;
    ASSERT_TRUE(WaitFor(first.tid, 1));
    ASSERT_TRUE(WaitFor(second.tid, 1));

    auto oldCookie = nativeCapture.Begin(1);
    ASSERT_NE(oldCookie, 0);
    ASSERT_TRUE(coop::detail::QueueStallSignal(first.tid.load(), oldCookie));
    nativeCapture.Cancel(oldCookie);

    auto cookie = nativeCapture.Begin(3);
    ASSERT_NE(cookie, 0);
    first.Join();
    EXPECT_EQ(nativeDelivered.load(), 1);
    EXPECT_EQ(nativeClaimed.load(), 0);
    EXPECT_FALSE(nativeCapture.Ready(cookie));

    ASSERT_TRUE(coop::detail::QueueStallSignal(second.tid.load(), cookie));
    second.Join();
    EXPECT_EQ(nativeClaimed.load(), 1);
    ASSERT_TRUE(nativeCapture.Ready(cookie));
    EXPECT_EQ(nativeCapture.depth, second.tid.load());
}

TEST(StallCaptureTest, ClaimedSlotSurvivesTimeoutUntilHandlerFinishes)
{
    NativeCaptureSignal handler;
    DelayedSignalThread target;
    ASSERT_TRUE(WaitFor(target.tid, 1));
    auto cookie = nativeCapture.Begin(1);
    ASSERT_NE(cookie, 0);
    nativePause.store(true);
    bool sent = coop::detail::QueueStallSignal(target.tid.load(), cookie);
    target.release.store(true);
    bool claimed = sent && WaitFor(nativeClaimed, 1);
    if (claimed)
    {
        nativeCapture.Cancel(cookie);
        EXPECT_EQ(nativeCapture.Begin(3), 0);
        EXPECT_FALSE(nativeCapture.Ready(cookie));
    }
    // Always release the handler before any fatal assertion or thread join.
    //
    nativePause.store(false, std::memory_order_release);
    target.Join();
    ASSERT_TRUE(sent);
    ASSERT_TRUE(claimed);
    ASSERT_TRUE(nativeCapture.Ready(cookie));
    EXPECT_EQ(nativeCapture.depth, target.tid.load());
    auto next = nativeCapture.Begin(3);
    EXPECT_NE(next, 0);
    nativeCapture.Cancel(next);
}

TEST(StallCaptureTest, UnrelatedSignalsCannotClaimRequest)
{
    coop::detail::StallCapture capture;
    auto cookie = capture.Begin(1);
    siginfo_t info{};
    info.si_value.sival_ptr = reinterpret_cast<void*>(cookie);
    info.si_pid = getpid();
    info.si_code = SI_TKILL;
    EXPECT_FALSE(capture.Claim(&info));
    info.si_code = SI_QUEUE;
    info.si_pid = 0;
    EXPECT_FALSE(capture.Claim(&info));
    info.si_pid = getpid();
    EXPECT_TRUE(capture.Claim(&info));
    capture.Complete(cookie);
    EXPECT_TRUE(capture.Ready(cookie));
}

TEST(StallDetectorTest, DelayedSignalCannotSupplyAnotherCooperatorsReport)
{
    coop::Cooperator first, second;
    std::atomic<int> firstReports{0}, secondReports{0}, secondPending{0};
    std::atomic<bool> releaseFirst{false}, releaseSecond{false};
    coop::StallDetector::Report secondReport{};
    coop::StallDetector firstDetector(&first, std::chrono::milliseconds(10),
        [&](const coop::StallDetector::Report&) { firstReports.fetch_add(1); });
    coop::StallDetector secondDetector(&second, std::chrono::milliseconds(10),
        [&](const coop::StallDetector::Report& report)
        {
            if (secondReports.load(std::memory_order_relaxed) == 0) secondReport = report;
            secondReports.fetch_add(1, std::memory_order_release);
        });

    coop::Thread firstThread(&first), secondThread(&second);

    auto stall = [](coop::Context* ctx, const char* name, std::atomic<bool>& release,
                    std::atomic<int>* pending)
    {
        ctx->SetName(name);
        sigset_t set, previous;
        sigemptyset(&set);
        sigaddset(&set, SIGURG);
        pthread_sigmask(SIG_BLOCK, &set, &previous);
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < until)
        {
            if (pending)
            {
                sigset_t signals;
                sigpending(&signals);
                if (sigismember(&signals, SIGURG)) pending->store(1, std::memory_order_release);
            }
            std::this_thread::yield();
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        ctx->GetCooperator()->Shutdown();
    };

    first.Submit([&](coop::Context* ctx) { stall(ctx, "expired-first", releaseFirst, nullptr); });
    bool firstTimedOut = WaitFor(firstReports, 1);
    second.Submit([&](coop::Context* ctx)
    {
        stall(ctx, "blocked-second", releaseSecond, &secondPending);
    });
    bool pending = WaitFor(secondPending, 1);
    releaseFirst.store(true, std::memory_order_release);
    bool secondTimedOut = WaitFor(secondReports, 1);
    releaseSecond.store(true, std::memory_order_release);
    // SubmitSync cannot be used to join contexts after shutdown; Thread joins at scope exit.
    // WaitFor's acquire pairs with publication of the one report for each unchanged stall.
    //
    EXPECT_TRUE(firstTimedOut);
    EXPECT_TRUE(pending);
    ASSERT_TRUE(secondTimedOut);
    EXPECT_EQ(secondReport.depth, 0);
    EXPECT_EQ(secondReport.context, nullptr);
    EXPECT_STREQ(secondReport.name, "");
}
