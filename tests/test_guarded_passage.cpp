#include <gtest/gtest.h>

#include "coop/chan/guarded_passage.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>

using namespace coop::chan;

// These tests use native threads: the passage is an SPSC ring independent of a
// cooperator, so its lifetime guarantees can be exercised without io_uring.
//
TEST(GuardedPassageTest, ConcurrentEndpointDestructionReachesShutdown)
{
    constexpr size_t ITERATIONS = 20000;
    std::barrier rendezvous(3);
    std::unique_ptr<RecvSide<int>> recv;
    std::unique_ptr<SendSide<int>> send;

    std::thread receiver([&]
    {
        for (size_t i = 0; i < ITERATIONS; ++i)
        {
            rendezvous.arrive_and_wait();
            recv.reset();
            rendezvous.arrive_and_wait();
        }
    });
    std::thread sender([&]
    {
        for (size_t i = 0; i < ITERATIONS; ++i)
        {
            rendezvous.arrive_and_wait();
            send.reset();
            rendezvous.arrive_and_wait();
        }
    });

    size_t incompleteShutdowns = 0;
    for (size_t i = 0; i < ITERATIONS; ++i)
    {
        FixedGuardedPassage<int, 4> passage;
        recv = std::make_unique<RecvSide<int>>(passage);
        send = std::make_unique<SendSide<int>>(passage);
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();

        auto state = passage.State();
        if (state != GuardedPassageState::Shutdown)
        {
            ++incompleteShutdowns;
            // Both endpoints are gone. Allow cleanup after recording the failure
            // so a regression reports an assertion instead of hanging the suite.
            //
            passage.TransitionTo(state, GuardedPassageState::Shutdown);
        }
    }
    receiver.join();
    sender.join();
    EXPECT_EQ(incompleteShutdowns, 0u);
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
