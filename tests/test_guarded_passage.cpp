#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#include "coop/chan/guarded_passage.h"

// GuardedPassage's destructor waits for both RecvSide and SendSide to have run
// their destructors (the handshake that drives GuardedPassageState to
// Shutdown). These tests exercise that wait directly, with no cooperator
// involved -- TransitionTo/State are plain atomics, so the peer-never-tore-down
// scenario (a context killed before running its side's destructor) is
// reproducible deterministically by simply never running one side's
// destructor, rather than by racing real context teardown.

using coop::chan::FixedGuardedPassage;
using coop::chan::GuardedPassageState;
using coop::chan::RecvSide;
using coop::chan::SendSide;

TEST(GuardedPassageTest, HealthyPairedTeardownIsImmediate)
{
    // Both sides tear down normally -- the paired/healthy path this change must
    // leave unchanged. Should complete essentially instantly, not wait anywhere
    // near the destructor's bounded deadline.
    //
    auto start = std::chrono::steady_clock::now();
    {
        FixedGuardedPassage<int, 4> passage;
        {
            RecvSide<int> recv(passage);
            SendSide<int> send(passage);
        }
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

#ifndef NDEBUG
// Red calibration for the destructor's bounded deadline: simulate a context
// that constructed its SendSide (so the passage committed to the SendRecv
// handshake) and then died without ever running that destructor -- the
// cluster cancel-path defect this change fixes at the consumer call site. With
// only coop's bound in place (no call-site fix), destroying the passage must
// not hang forever; in a debug build it fails loud instead.
//
TEST(GuardedPassageTest, DestructorAbortsWhenPeerSideNeverTearsDown)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";

    auto* passage = new FixedGuardedPassage<int, 4>();
    auto* recv = new RecvSide<int>(*passage);              // Created -> RecvOnly
    alignas(SendSide<int>) unsigned char sendStorage[sizeof(SendSide<int>)];
    new (sendStorage) SendSide<int>(*passage);              // RecvOnly -> SendRecv

    delete recv;  // SendRecv -> RecvShutdown: the surviving peer tears down
                   // normally. The SendSide placement-constructed above is
                   // deliberately never destroyed -- its destructor never runs,
                   // exactly as it would not for a context killed before
                   // returning through RunReaderFragmentDispatch.

    EXPECT_DEATH(
        { delete passage; },
        "the peer side \\(RecvSide/SendSide\\) never ran its destructor");
}
#endif

// A passage normally bridges two cooperators, so its two sides are routinely
// destroyed on two different threads at the same time. Both destructors used to
// read the state once and issue a single CAS: when both observed SendRecv, one
// CAS won and the loser's failed silently, parking the passage at SendShutdown
// or RecvShutdown with no side left alive to finish the handshake. The next
// ~GuardedPassage then waited out its full bound and failed loud -- a pairing
// bug reported against call sites that had paired correctly.
//
// Red calibration (single read-then-CAS): ~1700 of 20000 iterations park
// off-Shutdown on this host. Green (retrying destructors): zero. A parked
// passage is deliberately leaked rather than deleted -- deleting it would spend
// the destructor's full bound and then abort, hiding the count this test
// reports.
//
TEST(GuardedPassageTest, ConcurrentSideTeardownAlwaysReachesShutdown)
{
    constexpr int kIterations = 20000;
    int parked = 0;

    for (int i = 0; i < kIterations; i++)
    {
        auto* passage = new FixedGuardedPassage<int, 4>();
        auto* recv = new RecvSide<int>(*passage);   // Created  -> RecvOnly
        auto* send = new SendSide<int>(*passage);   // RecvOnly -> SendRecv

        std::atomic<int> ready{0};
        std::thread recvThread([&]
        {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < 2) {}
            delete recv;
        });
        std::thread sendThread([&]
        {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < 2) {}
            delete send;
        });
        recvThread.join();
        sendThread.join();

        if (passage->State() != GuardedPassageState::Shutdown)
        {
            parked++;
            continue;  // leaked on purpose; see comment above
        }
        delete passage;
    }

    EXPECT_EQ(parked, 0)
        << parked << " of " << kIterations
        << " concurrent teardowns lost a side transition and parked the passage "
           "short of Shutdown";
}
