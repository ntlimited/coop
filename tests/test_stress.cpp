#include <gtest/gtest.h>

#include "coop/channel.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/context.h"
#include "coop/signal.h"
#include "coop/self.h"
#include "test_helpers.h"

// Rapid spawn/kill: spawn a context, kill it, repeat. Exercises the full context lifecycle
// (allocate, enter, kill, exit, free) at high frequency. With guard pages enabled, any UAF
// in the kill/free path will SIGSEGV immediately.
//
TEST(StressTest, RapidSpawnKill)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int ITERATIONS = 1000;

        for (int i = 0; i < ITERATIONS; i++)
        {
            coop::Context::Handle handle;
            ctx->GetCooperator()->Spawn([](coop::Context* child)
            {
                child->Yield(true);
            }, &handle);

            handle.Kill();
            ctx->Yield(true);
        }
    });
}

// Wide fanout kill: spawn many children from one parent, then kill the parent. All children
// must be killed via Signal::Notify with the Steal fix. Tests that the stolen blocking list
// drains correctly when many waiters are present.
//
// Uses a Coordinator to synchronize: the parent holds it during spawning, releases it when
// all children are ready, then blocks on its own kill signal. The outer context Acquires the
// coordinator to block until spawning is complete.
//
TEST(StressTest, WideFanoutKill)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int NUM_CHILDREN = 500;

        coop::Context::Handle parentHandle;
        coop::Coordinator ready;
        int killCount = 0;

        ctx->GetCooperator()->Spawn([&](coop::Context* parent)
        {
            ready.TryAcquire(parent);

            for (int i = 0; i < NUM_CHILDREN; i++)
            {
                parent->GetCooperator()->Spawn([&](coop::Context* child)
                {
                    // Block on kill signal — this is the path through Signal::Notify
                    //
                    child->GetKilledSignal()->Wait(child);
                    killCount++;
                });
            }

            // Signal that all children are spawned and blocked
            //
            ready.Release(parent, false);
            parent->GetKilledSignal()->Wait(parent);
        }, &parentHandle);

        // Block until parent is done spawning
        //
        ready.Acquire(ctx);
        ready.Release(ctx, false);

        // Kill parent — cascades to all 500 children
        //
        parentHandle.Kill();

        // Yield until all children have observed their kill and incremented the counter
        //
        while (killCount < NUM_CHILDREN)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(killCount, NUM_CHILDREN);
    });
}

// Deep recursive kill: build a deeply nested context tree, then kill the root. Each level's
// kill propagates to its child before firing its own signal (the reorder fix in Context::Kill).
//
// The chain is built depth-first: each level spawns the next before blocking. A ready
// coordinator signals the outer context when the full chain is built.
//
TEST(StressTest, DeepRecursiveKill)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int DEPTH = 200;
        int deepestKilled = 0;

        coop::Context::Handle rootHandle;
        coop::Coordinator ready;

        std::function<void(coop::Context*, int)> buildChain;
        buildChain = [&](coop::Context* parent, int depth)
        {
            if (depth > DEPTH)
            {
                return;
            }

            parent->GetCooperator()->Spawn([&, depth](coop::Context* child)
            {
                buildChain(child, depth + 1);

                // Deepest child releases the ready coordinator
                //
                if (depth == DEPTH)
                {
                    ready.Release(child, false);
                }

                // Block on kill signal
                //
                child->GetKilledSignal()->Wait(child);
                deepestKilled = std::max(deepestKilled, depth);
            });
        };

        ctx->GetCooperator()->Spawn([&](coop::Context* root)
        {
            ready.TryAcquire(root);
            buildChain(root, 1);
            root->GetKilledSignal()->Wait(root);
        }, &rootHandle);

        // Block until the full chain is built
        //
        ready.Acquire(ctx);
        ready.Release(ctx, false);

        rootHandle.Kill();

        while (deepestKilled < DEPTH)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(deepestKilled, DEPTH);
    });
}

// Wide fanout with yielding children: same as WideFanoutKill but children yield instead of
// blocking on their kill signal. Tests that killing yielded (not blocked) contexts at scale
// works correctly.
//
TEST(StressTest, WideFanoutKillYielded)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int NUM_CHILDREN = 500;

        coop::Context::Handle parentHandle;
        coop::Coordinator ready;
        int killCount = 0;

        ctx->GetCooperator()->Spawn([&](coop::Context* parent)
        {
            ready.TryAcquire(parent);

            for (int i = 0; i < NUM_CHILDREN; i++)
            {
                parent->GetCooperator()->Spawn([&](coop::Context* child)
                {
                    while (!child->IsKilled())
                    {
                        child->Yield(true);
                    }
                    killCount++;
                });
            }

            ready.Release(parent, false);
            parent->GetKilledSignal()->Wait(parent);
        }, &parentHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        parentHandle.Kill();

        while (killCount < NUM_CHILDREN)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(killCount, NUM_CHILDREN);
    });
}

// Spawn/exit churn: rapidly spawn contexts that exit immediately, without explicit kill.
// Exercises the normal exit -> HandleCooperatorResumption EXITED -> FreeContext path at volume.
//
TEST(StressTest, SpawnExitChurn)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int ITERATIONS = 5000;
        int count = 0;

        for (int i = 0; i < ITERATIONS; i++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* child)
            {
                count++;
            });
        }

        EXPECT_EQ(count, ITERATIONS);
    });
}

// Channel shutdown with many blocked receivers. All receivers must wake up and observe
// shutdown. Exercises Coordinator::Release chains at scale.
//
TEST(StressTest, ChannelShutdownManyReceivers)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int NUM_RECEIVERS = 200;

        int buffer[4];
        coop::Channel<int> ch(ctx, buffer, 4);

        int wakeCount = 0;

        for (int i = 0; i < NUM_RECEIVERS; i++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* receiver)
            {
                int value = 0;
                bool result = ch.Recv(receiver, value);
                EXPECT_FALSE(result);
                wakeCount++;
            });
        }

        // All receivers are blocked on the empty channel
        //
        ch.Shutdown(ctx);

        while (wakeCount < NUM_RECEIVERS)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(wakeCount, NUM_RECEIVERS);
    });
}

// Channel shutdown with many blocked senders. Mirrors the receiver test but tests the
// send-side coordinator release chain.
//
TEST(StressTest, ChannelShutdownManySenders)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int NUM_SENDERS = 200;

        // Capacity 2 = 1 usable slot
        //
        int buffer[2];
        coop::Channel<int> ch(ctx, buffer, 2);

        // Fill the channel
        //
        ASSERT_TRUE(ch.TrySend(ctx, 0));

        int wakeCount = 0;

        for (int i = 0; i < NUM_SENDERS; i++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* sender)
            {
                bool result = ch.Send(sender, 42);
                EXPECT_FALSE(result);
                wakeCount++;
            });
        }

        // All senders are blocked on the full channel
        //
        ch.Shutdown(ctx);

        while (wakeCount < NUM_SENDERS)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(wakeCount, NUM_SENDERS);
    });
}

// Channel high throughput: many items flowing through a small channel with multiple
// producers and consumers. Tests the send/recv coordinator handoff at volume.
//
TEST(StressTest, ChannelHighThroughput)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int NUM_PRODUCERS = 10;
        constexpr int NUM_CONSUMERS = 10;
        constexpr int ITEMS_PER_PRODUCER = 1000;
        constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

        // Small buffer to maximize contention
        //
        int buffer[4];
        coop::Channel<int> ch(ctx, buffer, 4);

        std::atomic<int> produced{0};
        std::atomic<int> consumed{0};

        for (int p = 0; p < NUM_PRODUCERS; p++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* producer)
            {
                for (int i = 0; i < ITEMS_PER_PRODUCER; i++)
                {
                    ch.Send(producer, i);
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (int c = 0; c < NUM_CONSUMERS; c++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* consumer)
            {
                int value = 0;
                while (ch.Recv(consumer, value))
                {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        while (produced.load(std::memory_order_relaxed) < TOTAL_ITEMS)
        {
            ctx->Yield(true);
        }

        ch.Shutdown(ctx);

        while (consumed.load(std::memory_order_relaxed) < TOTAL_ITEMS)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(produced.load(), TOTAL_ITEMS);
        EXPECT_EQ(consumed.load(), TOTAL_ITEMS);
    });
}

// Mixed tree: wide parent with children at varying depths, some yielding, some blocked on
// signals. Kill the root and verify everything tears down cleanly.
//
TEST(StressTest, MixedTreeKill)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int BREADTH = 50;
        constexpr int DEPTH = 5;
        constexpr int EXPECTED_KILLS = BREADTH * DEPTH;

        coop::Context::Handle rootHandle;
        coop::Coordinator ready;
        std::atomic<int> killCount{0};

        ctx->GetCooperator()->Spawn([&](coop::Context* root)
        {
            ready.TryAcquire(root);

            for (int b = 0; b < BREADTH; b++)
            {
                std::function<void(coop::Context*, int)> buildBranch;
                buildBranch = [&](coop::Context* parent, int depth)
                {
                    if (depth > DEPTH)
                    {
                        return;
                    }

                    parent->GetCooperator()->Spawn([&, depth](coop::Context* child)
                    {
                        buildBranch(child, depth + 1);

                        // Alternate between yield-looping and signal-blocking
                        //
                        if (depth % 2 == 0)
                        {
                            child->GetKilledSignal()->Wait(child);
                        }
                        else
                        {
                            while (!child->IsKilled())
                            {
                                child->Yield(true);
                            }
                        }
                        killCount.fetch_add(1, std::memory_order_relaxed);
                    });
                };

                buildBranch(root, 1);
            }

            ready.Release(root, false);
            root->GetKilledSignal()->Wait(root);
        }, &rootHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        rootHandle.Kill();

        while (killCount.load(std::memory_order_relaxed) < EXPECTED_KILLS)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(killCount.load(), EXPECTED_KILLS);
    });
}

// Repeated shutdown cycles: create and tear down cooperators in a loop. Each cycle spawns
// contexts, shuts down, and verifies clean exit. Tests that no state leaks between cycles.
//
TEST(StressTest, RepeatedShutdownCycles)
{
    constexpr int CYCLES = 20;
    constexpr int CONTEXTS_PER_CYCLE = 50;

    for (int cycle = 0; cycle < CYCLES; cycle++)
    {
        test::RunInCooperator([&](coop::Context* ctx)
        {
            int count = 0;

            for (int i = 0; i < CONTEXTS_PER_CYCLE; i++)
            {
                ctx->GetCooperator()->Spawn([&](coop::Context* child)
                {
                    child->Yield(true);
                    count++;
                });
            }

            while (count < CONTEXTS_PER_CYCLE)
            {
                ctx->Yield(true);
            }

            EXPECT_EQ(count, CONTEXTS_PER_CYCLE);
        });
    }
}
