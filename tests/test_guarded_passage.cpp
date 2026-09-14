#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

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

namespace
{

struct RetryEvidence
{
    GuardedPassageState finalState;
    size_t reads;
    size_t attempts;
    std::vector<std::string> calls;
};

struct RecvTeardownRacePassage
{
    GuardedPassageState State()
    {
        ++m_reads;
        m_calls.push_back(m_state == GuardedPassageState::SendRecv
            ? "State SendRecv" : "State SendShutdown");
        return m_state;
    }

    bool TransitionTo(GuardedPassageState expected, GuardedPassageState next)
    {
        ++m_attempts;
        m_calls.push_back(expected == GuardedPassageState::SendRecv
            ? "Transition SendRecv->RecvShutdown"
            : "Transition SendShutdown->Shutdown");
        if (m_attempts == 1 && expected == GuardedPassageState::SendRecv &&
            next == GuardedPassageState::RecvShutdown)
        {
            m_state = GuardedPassageState::SendShutdown;
            return false;
        }
        if (m_state != expected)
            return false;
        m_state = next;
        return true;
    }

    RetryEvidence Evidence() const { return {m_state, m_reads, m_attempts, m_calls}; }

private:
    GuardedPassageState m_state = GuardedPassageState::SendRecv;
    size_t m_reads = 0;
    size_t m_attempts = 0;
    std::vector<std::string> m_calls;
};

struct SendTeardownRacePassage
{
    GuardedPassageState State()
    {
        ++m_reads;
        m_calls.push_back(m_state == GuardedPassageState::SendRecv
            ? "State SendRecv" : "State RecvShutdown");
        return m_state;
    }

    bool TransitionTo(GuardedPassageState expected, GuardedPassageState next)
    {
        ++m_attempts;
        m_calls.push_back(expected == GuardedPassageState::SendRecv
            ? "Transition SendRecv->SendShutdown"
            : "Transition RecvShutdown->Shutdown");
        if (m_attempts == 1 && expected == GuardedPassageState::SendRecv &&
            next == GuardedPassageState::SendShutdown)
        {
            m_state = GuardedPassageState::RecvShutdown;
            return false;
        }
        if (m_state != expected)
            return false;
        m_state = next;
        return true;
    }

    RetryEvidence Evidence() const { return {m_state, m_reads, m_attempts, m_calls}; }

private:
    GuardedPassageState m_state = GuardedPassageState::SendRecv;
    size_t m_reads = 0;
    size_t m_attempts = 0;
    std::vector<std::string> m_calls;
};

template<typename Teardown>
bool DetectRecvRetry(Teardown teardown, RetryEvidence& evidence)
{
    RecvTeardownRacePassage passage;
    teardown(passage);
    evidence = passage.Evidence();
    return evidence.finalState == GuardedPassageState::Shutdown &&
           evidence.reads == 2 &&
           evidence.attempts == 2 &&
           evidence.calls == std::vector<std::string>{
               "State SendRecv",
               "Transition SendRecv->RecvShutdown",
               "State SendShutdown",
               "Transition SendShutdown->Shutdown",
           };
}

template<typename Teardown>
bool DetectSendRetry(Teardown teardown, RetryEvidence& evidence)
{
    SendTeardownRacePassage passage;
    teardown(passage);
    evidence = passage.Evidence();
    return evidence.finalState == GuardedPassageState::Shutdown &&
           evidence.reads == 2 &&
           evidence.attempts == 2 &&
           evidence.calls == std::vector<std::string>{
               "State SendRecv",
               "Transition SendRecv->SendShutdown",
               "State RecvShutdown",
               "Transition RecvShutdown->Shutdown",
           };
}

template<typename Passage>
void HistoricalOneShotRecvTeardown(Passage& passage)
{
    auto s = passage.State();
    if (s == GuardedPassageState::RecvOnly)
    {
        passage.TransitionTo(GuardedPassageState::RecvOnly, GuardedPassageState::Shutdown);
        return;
    }
    if (s == GuardedPassageState::SendRecv)
    {
        passage.TransitionTo(GuardedPassageState::SendRecv, GuardedPassageState::RecvShutdown);
        return;
    }
    if (s == GuardedPassageState::SendShutdown)
        passage.TransitionTo(GuardedPassageState::SendShutdown, GuardedPassageState::Shutdown);
}

template<typename Passage>
void HistoricalOneShotSendTeardown(Passage& passage)
{
    auto s = passage.State();
    if (s == GuardedPassageState::SendRecv)
    {
        passage.TransitionTo(GuardedPassageState::SendRecv, GuardedPassageState::SendShutdown);
        return;
    }
    if (s == GuardedPassageState::RecvShutdown)
        passage.TransitionTo(GuardedPassageState::RecvShutdown, GuardedPassageState::Shutdown);
}

} // namespace

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

TEST(GuardedPassageTest, RecvRetryDetectorAcceptsHelperAndRejectsOneShotTeardown)
{
    RetryEvidence good;
    EXPECT_TRUE(DetectRecvRetry([](auto& passage)
    {
        coop::chan::detail::CompleteRecvTeardown(passage);
    }, good));
    EXPECT_EQ(good.reads, 2u);
    EXPECT_EQ(good.attempts, 2u);
    EXPECT_EQ(good.finalState, GuardedPassageState::Shutdown);

    RetryEvidence historical;
    EXPECT_FALSE(DetectRecvRetry(HistoricalOneShotRecvTeardown<RecvTeardownRacePassage>,
                                 historical));
    EXPECT_EQ(historical.reads, 1u);
    EXPECT_EQ(historical.attempts, 1u);
    EXPECT_EQ(historical.finalState, GuardedPassageState::SendShutdown);
}

TEST(GuardedPassageTest, SendRetryDetectorAcceptsHelperAndRejectsOneShotTeardown)
{
    RetryEvidence good;
    EXPECT_TRUE(DetectSendRetry([](auto& passage)
    {
        coop::chan::detail::CompleteSendTeardown(passage);
    }, good));
    EXPECT_EQ(good.reads, 2u);
    EXPECT_EQ(good.attempts, 2u);
    EXPECT_EQ(good.finalState, GuardedPassageState::Shutdown);

    RetryEvidence historical;
    EXPECT_FALSE(DetectSendRetry(HistoricalOneShotSendTeardown<SendTeardownRacePassage>,
                                 historical));
    EXPECT_EQ(historical.reads, 1u);
    EXPECT_EQ(historical.attempts, 1u);
    EXPECT_EQ(historical.finalState, GuardedPassageState::RecvShutdown);
}

// This integration coverage establishes each partial state directly, then
// lets the real endpoint destructors complete the complementary transition.
// The deterministic fake tests above cover the concurrent stale-read race.
//
TEST(GuardedPassageTest, RealDestructorsCompleteEstablishedPartialStates)
{
    {
        FixedGuardedPassage<int, 4> passage;
        {
            RecvSide<int> recv(passage);
            SendSide<int> send(passage);
            auto senderObserved = passage.State();
            auto receiverObserved = passage.State();
            ASSERT_EQ(senderObserved, GuardedPassageState::SendRecv);
            ASSERT_EQ(receiverObserved, GuardedPassageState::SendRecv);

            ASSERT_TRUE(passage.TransitionTo(senderObserved, GuardedPassageState::SendShutdown));
            EXPECT_FALSE(passage.TransitionTo(receiverObserved, GuardedPassageState::RecvShutdown));
            EXPECT_EQ(passage.State(), GuardedPassageState::SendShutdown);
        }
        EXPECT_EQ(passage.State(), GuardedPassageState::Shutdown);
    }

    {
        FixedGuardedPassage<int, 4> passage;
        {
            RecvSide<int> recv(passage);
            SendSide<int> send(passage);
            auto receiverObserved = passage.State();
            auto senderObserved = passage.State();
            ASSERT_EQ(receiverObserved, GuardedPassageState::SendRecv);
            ASSERT_EQ(senderObserved, GuardedPassageState::SendRecv);

            ASSERT_TRUE(passage.TransitionTo(receiverObserved, GuardedPassageState::RecvShutdown));
            EXPECT_FALSE(passage.TransitionTo(senderObserved, GuardedPassageState::SendShutdown));
            EXPECT_EQ(passage.State(), GuardedPassageState::RecvShutdown);
        }
        EXPECT_EQ(passage.State(), GuardedPassageState::Shutdown);
    }
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
