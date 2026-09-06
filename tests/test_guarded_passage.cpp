#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <semaphore>
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

// A peer that never finishes teardown must fail within the bound in every build.
// Returning from the destructor would reclaim memory the peer can still access.
//
TEST(GuardedPassageTest, DestructorAbortsWhenPeerSideNeverTearsDown)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";

    using Passage = FixedGuardedPassage<int, 4>;
    EXPECT_DEATH(
        {
            auto* passage = new Passage();
            auto* recv = new RecvSide<int>(*passage);
            alignas(SendSide<int>) unsigned char sendStorage[sizeof(SendSide<int>)];
            new (sendStorage) SendSide<int>(*passage);
            delete recv;
            // Leave the sender alive while attempting to reclaim its passage.
            delete passage;
        },
        "destructor timed out waiting for Shutdown");
}

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

namespace
{

struct DestructionState
{
    std::mutex mutex;
    std::condition_variable changed;
    size_t destroyed = 0;
};

struct TrackedValue
{
    explicit TrackedValue(DestructionState& state) : m_state(state) {}
    ~TrackedValue()
    {
        std::lock_guard lock(m_state.mutex);
        ++m_state.destroyed;
        m_state.changed.notify_one();
    }
    DestructionState& m_state;
};

} // namespace

TEST(GuardedPassageTest, StorageLivesUntilBothEndpointsAreDestroyed)
{
    for (bool senderFirst : {false, true})
    {
        SCOPED_TRACE(senderFirst ? "sender first" : "receiver first");
        using Value = std::unique_ptr<TrackedValue>;
        DestructionState state;
        auto passage = std::make_unique<FixedGuardedPassage<Value, 4>>();
        auto recv = std::make_unique<RecvSide<Value>>(*passage);
        auto send = std::make_unique<SendSide<Value>>(*passage);
        for (size_t i = 0; i < 4; ++i)
            ASSERT_TRUE(send->TryPush(std::make_unique<TrackedValue>(state)));

        std::binary_semaphore deleting(0);
        std::atomic<bool> finished{false};
        std::thread owner([&]
        {
            deleting.release();
            passage.reset();
            finished.store(true);
        });
        deleting.acquire();

        auto expectStorageAlive = [&]
        {
            // Give premature element destruction a bounded chance to report itself.
            // Endpoints stay live throughout the wait, regardless of thread scheduling.
            //
            std::unique_lock lock(state.mutex);
            EXPECT_FALSE(state.changed.wait_for(lock, std::chrono::milliseconds(50), [&]
            {
                return state.destroyed != 0;
            }));
            EXPECT_FALSE(finished.load());
        };

        expectStorageAlive();
        if (senderFirst) send.reset();
        else recv.reset();
        expectStorageAlive();
        if (senderFirst) recv.reset();
        else send.reset();

        owner.join();
        EXPECT_TRUE(finished.load());
        EXPECT_EQ(state.destroyed, 4u);
    }
}

TEST(GuardedPassageTest, UnusedAndReceiverOnlyPassagesCanBeDestroyed)
{
    FixedGuardedPassage<int, 4> unused;
    EXPECT_EQ(unused.State(), GuardedPassageState::Created);

    FixedGuardedPassage<int, 4> receiverOnly;
    {
        RecvSide<int> recv(receiverOnly);
        EXPECT_EQ(receiverOnly.State(), GuardedPassageState::RecvOnly);
    }
    EXPECT_TRUE(receiverOnly.IsShutdown());
}

TEST(GuardedPassageTest, SenderShutdownPreservesQueuedValues)
{
    FixedGuardedPassage<int, 4> passage;
    RecvSide<int> recv(passage);
    {
        SendSide<int> send(passage);
        for (int i = 0; i < 4; ++i)
            EXPECT_TRUE(send.TryPush(i));
        EXPECT_FALSE(send.TryPush(4));
    }
    EXPECT_TRUE(recv.SenderDone());
    for (int i = 0; i < 4; ++i)
    {
        int value = -1;
        EXPECT_TRUE(recv.TryPop(value));
        EXPECT_EQ(value, i);
    }
    EXPECT_TRUE(recv.IsEmpty());
}
