#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

#include "coop/context.h"
#include "coop/cooperator.h"
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
