#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <semaphore>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/countdown_latch.h"
#include "coop/cooperate.h"
#include "coop/continuation.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "test_helpers.h"

TEST(CountdownLatchTest, ArrivalBeforeWaitPublishesAllPredecessors)
{
    coop::CountdownLatch latch(8);
    std::array<int, 8> values{};
    std::array<std::thread, 8> threads;
    for (size_t i = 0; i < threads.size(); ++i)
        threads[i] = std::thread(
            [&, i]
            {
                values[i] = int(i + 1);
                latch.Arrive();
            });
    for (auto& thread : threads)
        thread.join();
    test::RunInCooperator(
        [&](coop::Context* ctx)
        {
            latch.Wait(ctx);
            for (size_t i = 0; i < values.size(); ++i)
                EXPECT_EQ(values[i], i + 1);
        });
}

TEST(CountdownLatchTest, WaiterBeforeForeignCooperatorArrival)
{
    coop::Cooperator peer;
    coop::Thread thread(&peer);
    test::RunInCooperator(
        [&](coop::Context* ctx)
        {
            coop::CountdownLatch latch(1);
            bool complete = false;
            ASSERT_TRUE(ctx->GetCooperator()->Spawn(
                [&](coop::Context* child)
                {
                    latch.Wait(child);
                    complete = true;
                }));
            // Spawn returned only after its child suspended on the empty latch.
            EXPECT_FALSE(complete);
            ASSERT_TRUE(peer.Submit([&](coop::Context*) { latch.Arrive(); }));
            while (!complete)
                ctx->Yield(true);
        });
    peer.Shutdown();
}

TEST(CountdownLatchTest, EmptyRoundAndAlreadyKilledWait)
{
    test::RunInCooperator(
        [](coop::Context* ctx)
        {
            coop::CountdownLatch latch;
            latch.Wait(ctx);
            ctx->KillSelf(coop::KillCause::Kill);
            uint32_t cancelled = 0;
            EXPECT_EQ(latch.WaitKill(
                          ctx,
                          [](void* state, coop::CountdownLatch::WaitStatus)
                          { ++*static_cast<uint32_t*>(state); },
                          &cancelled),
                      coop::CountdownLatch::WaitStatus::Killed);
            EXPECT_EQ(cancelled, 1u);
            latch.Wait(ctx);
        });
}

TEST(CountdownLatchTest, ManyLatchesAcrossCooperatorsAndReset)
{
    std::array<coop::Cooperator, 4> peers;
    std::array<coop::Thread, 4> threads{{&peers[0], &peers[1], &peers[2], &peers[3]}};
    test::RunInCooperator(
        [&](coop::Context* ctx)
        {
            std::array<coop::CountdownLatch, 64> latches;
            std::array<std::array<int, 4>, 64> values{};
            for (uint32_t round = 0; round < 4; ++round)
            {
                for (auto& latch : latches)
                    latch.Reset(peers.size());
                for (size_t peer = 0; peer < peers.size(); ++peer)
                    ASSERT_TRUE(peers[peer].Submit(
                        [&, peer, round](coop::Context*)
                        {
                            for (size_t i = 0; i < latches.size(); ++i)
                            {
                                values[i][peer] = int(round + peer + i);
                                latches[i].Arrive();
                            }
                        }));
                for (size_t i = 0; i < latches.size(); ++i)
                {
                    latches[i].Wait(ctx);
                    for (size_t peer = 0; peer < peers.size(); ++peer)
                        EXPECT_EQ(values[i][peer], round + peer + i);
                }
            }
        },
        {.priority = 0, .stackSize = 65536});
    for (auto& peer : peers)
        peer.Shutdown();
}

TEST(CountdownLatchTest, CancellationNotifiesThenDrainsAcceptedArrivals)
{
    test::RunInCooperator(
        [](coop::Context* ctx)
        {
            coop::CountdownLatch latch(1);
            std::atomic<bool> cancelled{false};
            std::thread arriver(
                [&]
                {
                    while (!cancelled.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    latch.Arrive();
                });
            bool finished = false;
            coop::Context::Handle waiter;
            ASSERT_TRUE(ctx->GetCooperator()->Spawn(
                [&](coop::Context* child)
                {
                    EXPECT_EQ(latch.WaitKill(
                                  child,
                                  [](void* state, coop::CountdownLatch::WaitStatus) {
                                      static_cast<std::atomic<bool>*>(state)->store(
                                          true, std::memory_order_release);
                                  },
                                  &cancelled),
                              coop::CountdownLatch::WaitStatus::Killed);
                    EXPECT_TRUE(latch.IsReady());
                    finished = true;
                },
                &waiter));
            waiter.Kill();
            // The arriving thread cannot proceed until cancellation reaches the waiter.
            while (!finished)
                ctx->Yield(true);
            arriver.join();
        });
}

TEST(CountdownLatchTest, ShutdownDuringWaitAndWaitAfterShutdown)
{
    for (uint32_t repetition = 0; repetition < 32; ++repetition)
        for (bool waitAfterShutdown : {false, true})
        {
            coop::Cooperator owner;
            coop::Thread ownerThread(&owner);
            ASSERT_TRUE(owner.Submit(
                [&](coop::Context* ctx)
                {
                    coop::CountdownLatch latch(1);
                    std::atomic<bool> started{false};
                    if (waitAfterShutdown)
                    {
                        owner.Shutdown();
                        // Let the killed wake reader park before registering a cleanup wait.
                        for (int i = 0; i < 8; ++i)
                            ctx->Yield(true);
                    }
                    std::thread arriver(
                        [&]
                        {
                            while (!started.load(std::memory_order_acquire))
                                std::this_thread::yield();
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            owner.Shutdown();
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            latch.Arrive();
                        });
                    started.store(true, std::memory_order_release);
                    latch.Wait(ctx); // unkillable join must survive shutdown
                    EXPECT_TRUE(latch.IsReady());
                    arriver.join();
                }));
        }
}

TEST(CountdownLatchTest, ShutdownRetainsPendingCooperateReply)
{
    coop::Cooperator remote;
    coop::Thread remoteThread(&remote);
    std::binary_semaphore entered(0), release(0);
    ASSERT_TRUE(remote.Submit(
        [&](coop::Context*)
        {
            entered.release();
            release.acquire(); // deliberately hold the remote scheduler before dispatch
        }));
    entered.acquire();
    coop::Cooperator owner;
    std::atomic<bool> waiting{false}, stopped{false}, completed{false};
    std::thread ownerThread(
        [&]
        {
            owner.Launch();
            stopped.store(true, std::memory_order_release);
        });
    EXPECT_TRUE(owner.Submit(
        [&](coop::Context* ctx)
        {
            coop::CooperateHandle reply(ctx);
            EXPECT_TRUE(remote.Cooperate([](coop::Context*) {}, &reply));
            waiting.store(true, std::memory_order_release);
            EXPECT_TRUE(reply.Wait(ctx));
            completed.store(true, std::memory_order_release);
        }));
    while (!waiting.load(std::memory_order_acquire))
        std::this_thread::yield();
    owner.Shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(stopped.load(std::memory_order_acquire));
    release.release();
    EXPECT_TRUE(remote.SubmitSync([](coop::Context*) {}));
    ownerThread.join();
    EXPECT_TRUE(completed.load(std::memory_order_acquire));
    remote.Shutdown();
}

namespace
{
void CleanupAfterQueuedContinuation()
{
    test::RunInCooperator(
        [](coop::Context* ctx)
        {
            auto* owner = ctx->GetCooperator();
            owner->m_drainBudget = 1;
            owner->m_drainPeekStride = 0;
            owner->Shutdown();
            // Begin cleanup after the shutdown kill sweep; the wake transport stays retained.
            while (!ctx->IsKilled())
                ctx->Yield(true);
            std::array<coop::Coordinator, 64> gates;
            for (auto& gate : gates)
                EXPECT_TRUE(gate.TryAcquire());
            auto tail = gates.back().Continue([](coop::Coordinator*) {});
            for (size_t i = 0; i + 1 < gates.size(); ++i)
                gates[i].ContinueDetached([&, i](coop::Coordinator*)
                                          { gates[i + 1].Release(ctx, false); });
            gates.front().Release(ctx, false);
            tail.Await();
            coop::CountdownLatch cleanup(1);
            std::thread arriver(
                [&]
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    cleanup.Arrive();
                });
            cleanup.Wait(ctx);
            arriver.join();
        });
}
} // namespace

TEST(CountdownLatchTest, QueuedContinuationCanBeginCleanupAfterShutdown)
{
    EXPECT_EXIT(
        {
            ::alarm(5);
            CleanupAfterQueuedContinuation();
            _Exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST(CountdownLatchTest, DeadlineCancelsThenDrainsAcceptedArrival)
{
    test::RunInCooperator(
        [](coop::Context* ctx)
        {
            coop::CountdownLatch latch(1);
            std::atomic<bool> cancelled{false};
            std::thread arriver(
                [&]
                {
                    while (!cancelled.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    latch.Arrive();
                });
            ctx->SetDeadlineIn(std::chrono::milliseconds(2));
            auto const status = latch.WaitKill(
                ctx,
                [](void* state, coop::CountdownLatch::WaitStatus)
                { static_cast<std::atomic<bool>*>(state)->store(true, std::memory_order_release); },
                &cancelled);
            EXPECT_EQ(status, coop::CountdownLatch::WaitStatus::Killed);
            EXPECT_EQ(ctx->WhyKilled(), coop::KillCause::Deadline);
            EXPECT_TRUE(latch.IsReady());
            arriver.join();
        });
}

TEST(CountdownLatchTest, TerminalArrivalAfterRegistrationBeforeSuspension)
{
    test::RunInCooperator(
        [](coop::Context* ctx)
        {
            coop::CountdownLatch latch(1);
            bool registered = false;
            struct State
            {
                coop::CountdownLatch& latch;
                bool& registered;
            } state{latch, registered};
            EXPECT_EQ(latch.WaitKill(ctx, nullptr, &state,
                                     [](void* pointer)
                                     {
                                         auto& state = *static_cast<State*>(pointer);
                                         state.registered = true;
                                         std::thread arrival([&] { state.latch.Arrive(); });
                                         arrival.join();
                                         // Deliver the queued notification before the wait
                                         // suspends.
                                         coop::Self()->Yield(true);
                                     }),
                      coop::CountdownLatch::WaitStatus::Completed);
            EXPECT_TRUE(registered);
            EXPECT_TRUE(latch.IsReady());
        });
}
