#include <gtest/gtest.h>

#include "coop/channel.h"
#include "coop/self.h"
#include "test_helpers.h"

TEST(ChannelTest, SendRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[4];
        coop::Channel<int> ch(ctx, buffer, 4);

        ctx->GetCooperator()->Spawn([&](coop::Context* sender)
        {
            ch.Send(sender, 42);
        });

        int value = 0;
        ch.Recv(ctx, value);
        EXPECT_EQ(value, 42);
    });
}

TEST(ChannelTest, TrySendFull)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Capacity 2 means buffer size 2, but ring buffer uses one slot as sentinel,
        // so effectively 1 usable slot
        int buffer[2];
        coop::Channel<int> ch(ctx, buffer, 2);

        // First send should succeed (fills the one usable slot)
        EXPECT_TRUE(ch.TrySend(ctx, 1));

        // Second should fail (full)
        EXPECT_FALSE(ch.TrySend(ctx, 2));
    });
}

TEST(ChannelTest, TryRecvEmpty)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[4];
        coop::Channel<int> ch(ctx, buffer, 4);

        int value = 0;
        EXPECT_FALSE(ch.TryRecv(ctx, value));
    });
}

TEST(ChannelTest, Shutdown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[4];
        coop::Channel<int> ch(ctx, buffer, 4);

        EXPECT_FALSE(ch.IsShutdown());
        ch.Shutdown(ctx);
        EXPECT_TRUE(ch.IsShutdown());
    });
}

// Sender blocks when channel is full, unblocks when receiver consumes.
//
TEST(ChannelTest, SendBlocksWhenFull)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Capacity 2 = 1 usable slot (ring buffer sentinel)
        //
        int buffer[2];
        coop::Channel<int> ch(ctx, buffer, 2);

        int step = 0;

        // Fill the channel
        //
        EXPECT_TRUE(ch.TrySend(ctx, 10));

        // Spawn a sender that will block because the channel is full
        //
        ctx->GetCooperator()->Spawn([&](coop::Context* sender)
        {
            step = 1;
            ch.Send(sender, 20);
            // Unblocked after receiver drains
            //
            step = 3;
        });

        EXPECT_EQ(step, 1);
        step = 2;

        // Consume to unblock the sender
        //
        int value = 0;
        ch.Recv(ctx, value);
        EXPECT_EQ(value, 10);

        // The sender should have run and completed
        //
        EXPECT_EQ(step, 3);

        // Drain the second value the sender pushed
        //
        ch.Recv(ctx, value);
        EXPECT_EQ(value, 20);
    });
}

// Receiver blocks when channel is empty, unblocks when sender produces.
//
TEST(ChannelTest, RecvBlocksWhenEmpty)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[4];
        coop::Channel<int> ch(ctx, buffer, 4);

        int step = 0;
        int received = 0;

        // Spawn a receiver that will block because the channel is empty
        //
        ctx->GetCooperator()->Spawn([&](coop::Context* receiver)
        {
            step = 1;
            ch.Recv(receiver, received);
            // Unblocked after sender produces
            //
            step = 3;
        });

        EXPECT_EQ(step, 1);
        step = 2;

        // Send to unblock the receiver
        //
        ch.Send(ctx, 42);

        EXPECT_EQ(step, 3);
        EXPECT_EQ(received, 42);
    });
}

// Receiver blocked on empty channel. Shutdown unblocks it and Recv returns false.
//
TEST(ChannelTest, ShutdownWakesBlockedRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[4];
        coop::Channel<int> ch(ctx, buffer, 4);

        bool recvResult = true;

        ctx->GetCooperator()->Spawn([&](coop::Context* receiver)
        {
            int value = 0;
            recvResult = ch.Recv(receiver, value);
        });

        // Receiver is now blocked on the empty channel. Shut it down.
        //
        ch.Shutdown(ctx);

        EXPECT_FALSE(recvResult);
    });
}

// Sender blocked on full channel. Shutdown unblocks it and Send returns false.
//
TEST(ChannelTest, ShutdownWakesBlockedSend)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Capacity 2 = 1 usable slot
        //
        int buffer[2];
        coop::Channel<int> ch(ctx, buffer, 2);

        // Fill the channel
        //
        EXPECT_TRUE(ch.TrySend(ctx, 10));

        bool sendResult = true;

        ctx->GetCooperator()->Spawn([&](coop::Context* sender)
        {
            sendResult = ch.Send(sender, 20);
        });

        // Sender is now blocked on the full channel. Shut it down.
        //
        ch.Shutdown(ctx);

        EXPECT_FALSE(sendResult);
    });
}

// Multiple senders and receivers operating concurrently on the same channel.
//
TEST(ChannelTest, MultipleProducersConsumers)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[8];
        coop::Channel<int> ch(ctx, buffer, 8);

        constexpr int NUM_PRODUCERS = 3;
        constexpr int ITEMS_PER_PRODUCER = 10;
        constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

        int produced = 0;
        int consumed = 0;
        int sum = 0;

        // Spawn producers
        //
        for (int p = 0; p < NUM_PRODUCERS; p++)
        {
            ctx->GetCooperator()->Spawn([&, p](coop::Context* producer)
            {
                for (int i = 0; i < ITEMS_PER_PRODUCER; i++)
                {
                    int value = p * ITEMS_PER_PRODUCER + i;
                    ch.Send(producer, value);
                    produced++;
                }
            });
        }

        // Spawn consumers — two consumers that pull items until channel is shutdown
        //
        constexpr int NUM_CONSUMERS = 2;
        for (int c = 0; c < NUM_CONSUMERS; c++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* consumer)
            {
                int value = 0;
                while (ch.Recv(consumer, value))
                {
                    consumed++;
                    sum += value;
                }
            });
        }

        // Let everyone run until producers are done
        //
        while (produced < TOTAL_ITEMS)
        {
            ctx->Yield(true);
        }

        // Shut down the channel so consumers exit their Recv loops
        //
        ch.Shutdown(ctx);

        // Let consumers finish draining
        //
        while (consumed < TOTAL_ITEMS)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(produced, TOTAL_ITEMS);
        EXPECT_EQ(consumed, TOTAL_ITEMS);

        // Sum of 0..29 = 435
        //
        int expectedSum = (TOTAL_ITEMS * (TOTAL_ITEMS - 1)) / 2;
        EXPECT_EQ(sum, expectedSum);
    });
}
