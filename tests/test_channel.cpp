#include <gtest/gtest.h>

#include "coop/channel.h"
#include "coop/select.h"
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
            ch.Send(42);
        });

        int value = 0;
        ch.Recv(value);
        EXPECT_EQ(value, 42);
    });
}

TEST(ChannelTest, TrySendFull)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[2];
        coop::Channel<int> ch(ctx, buffer, 2);

        EXPECT_TRUE(ch.TrySend(1));
        EXPECT_TRUE(ch.TrySend(2));
        EXPECT_FALSE(ch.TrySend(3));
    });
}

TEST(ChannelTest, TryRecvEmpty)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[4];
        coop::Channel<int> ch(ctx, buffer, 4);

        int value = 0;
        EXPECT_FALSE(ch.TryRecv(value));
    });
}

TEST(ChannelTest, Shutdown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[4];
        coop::Channel<int> ch(ctx, buffer, 4);

        EXPECT_FALSE(ch.IsShutdown());
        ch.Shutdown();
        EXPECT_TRUE(ch.IsShutdown());
    });
}

// Sender blocks when channel is full, unblocks when receiver consumes.
//
TEST(ChannelTest, SendBlocksWhenFull)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[1];
        coop::Channel<int> ch(ctx, buffer, 1);

        int step = 0;

        // Fill the channel
        //
        EXPECT_TRUE(ch.TrySend(10));

        // Spawn a sender that will block because the channel is full
        //
        ctx->GetCooperator()->Spawn([&](coop::Context* sender)
        {
            step = 1;
            ch.Send(20);
            // Unblocked after receiver drains
            //
            step = 3;
        });

        EXPECT_EQ(step, 1);
        step = 2;

        // Consume to unblock the sender
        //
        int value = 0;
        ch.Recv(value);
        EXPECT_EQ(value, 10);

        // The sender should have run and completed
        //
        EXPECT_EQ(step, 3);

        // Drain the second value the sender pushed
        //
        ch.Recv(value);
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
            ch.Recv(received);
            // Unblocked after sender produces
            //
            step = 3;
        });

        EXPECT_EQ(step, 1);
        step = 2;

        // Send to unblock the receiver
        //
        ch.Send(42);

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
            recvResult = ch.Recv(value);
        });

        // Receiver is now blocked on the empty channel. Shut it down.
        //
        ch.Shutdown();

        EXPECT_FALSE(recvResult);
    });
}

// Sender blocked on full channel. Shutdown unblocks it and Send returns false.
//
TEST(ChannelTest, ShutdownWakesBlockedSend)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[1];
        coop::Channel<int> ch(ctx, buffer, 1);

        // Fill the channel
        //
        EXPECT_TRUE(ch.TrySend(10));

        bool sendResult = true;

        ctx->GetCooperator()->Spawn([&](coop::Context* sender)
        {
            sendResult = ch.Send(20);
        });

        // Sender is now blocked on the full channel. Shut it down.
        //
        ch.Shutdown();

        EXPECT_FALSE(sendResult);
    });
}

// SendAll pushes a batch; partial send returns false on shutdown mid-batch.
//
TEST(ChannelTest, SendAll)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[4];
        coop::Channel<int> ch(ctx, buf, 4);

        const int data[] = { 10, 20, 30, 40 };
        EXPECT_TRUE(ch.SendAll(data, 4));

        int v;
        for (int expected : data)
        {
            EXPECT_TRUE(ch.TryRecv(v));
            EXPECT_EQ(v, expected);
        }
    });
}

// SendAll blocks when buffer is full and resumes when consumer drains.
//
TEST(ChannelTest, SendAllBlocks)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[2];
        coop::Channel<int> ch(ctx, buf, 2);

        int recvd[4] = {};
        int nRecvd = 0;

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            int v;
            while (ch.Recv(v))
                recvd[nRecvd++] = v;
        });

        const int data[] = { 1, 2, 3, 4 };
        EXPECT_TRUE(ch.SendAll(data, 4));
        ch.Shutdown();

        while (nRecvd < 4)
            ctx->Yield(true);

        for (int i = 0; i < 4; i++)
            EXPECT_EQ(recvd[i], data[i]);
    });
}

// Drain pulls available items without blocking; returns 0 on empty channel.
//
TEST(ChannelTest, Drain)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[4];
        coop::Channel<int> ch(ctx, buf, 4);

        EXPECT_TRUE(ch.TrySend(1));
        EXPECT_TRUE(ch.TrySend(2));
        EXPECT_TRUE(ch.TrySend(3));

        int out[4];
        size_t n = ch.Drain(out, 4);
        EXPECT_EQ(n, 3u);
        EXPECT_EQ(out[0], 1);
        EXPECT_EQ(out[1], 2);
        EXPECT_EQ(out[2], 3);

        // Channel is now empty — Drain returns 0.
        //
        EXPECT_EQ(ch.Drain(out, 4), 0u);
    });
}

// Drain respects maxCount and leaves remaining items in the channel.
//
TEST(ChannelTest, DrainPartial)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[4];
        coop::Channel<int> ch(ctx, buf, 4);

        for (int i = 1; i <= 4; i++)
            EXPECT_TRUE(ch.TrySend(i));

        int out[4];
        size_t n = ch.Drain(out, 2);
        EXPECT_EQ(n, 2u);
        EXPECT_EQ(out[0], 1);
        EXPECT_EQ(out[1], 2);

        // Two items remain.
        //
        int v;
        EXPECT_TRUE(ch.TryRecv(v));
        EXPECT_EQ(v, 3);
        EXPECT_TRUE(ch.TryRecv(v));
        EXPECT_EQ(v, 4);
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
                    ch.Send(value);
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
                while (ch.Recv(value))
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
        ch.Shutdown();

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

// Select across two channels: block on whichever becomes ready first.
//
TEST(ChannelTest, SelectRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf1[4], buf2[4];
        coop::Channel<int> ch1(ctx, buf1, 4);
        coop::Channel<int> ch2(ctx, buf2, 4);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < 4; i++) ch1.Send(i * 10);
            ch1.Shutdown();
        });

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < 4; i++) ch2.Send(i * 100);
            ch2.Shutdown();
        });

        int sum = 0, count = 0;
        bool done1 = false, done2 = false;

        auto on1 = coop::On(ch1, [&](int v) { sum += v; count++; }, [&]{ done1 = true; });
        auto on2 = coop::On(ch2, [&](int v) { sum += v; count++; }, [&]{ done2 = true; });

        while (!done1 || !done2)
        {
            if      (!done1 && !done2) coop::Select(ctx, on1, on2);
            else if (!done1)           coop::Select(ctx, on1);
            else                       coop::Select(ctx, on2);
        }

        // sum of (0+10+20+30) + (0+100+200+300) = 60 + 600 = 660
        //
        EXPECT_EQ(count, 8);
        EXPECT_EQ(sum, 660);
    });
}

// Select: shutdown on either channel is detected and handled cleanly.
//
TEST(ChannelTest, SelectRecvShutdown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf1[4], buf2[4];
        coop::Channel<int> ch1(ctx, buf1, 4);
        coop::Channel<int> ch2(ctx, buf2, 4);

        // ch1 gets one item then shuts down; ch2 is never written to.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch1.Send(42);
            ch1.Shutdown();
        });

        int received = 0;

        // Loop until Select returns false (ch1 shutdown fires).
        //
        while (coop::Select(ctx,
            coop::On(ch1, [&](int v) { received = v; }),
            coop::On(ch2, [&](int v) { (void)v; })
        )) {}

        EXPECT_EQ(received, 42);

        ch2.Shutdown();
    });
}

// FixedChannel — owns its buffer as a member; otherwise identical to Channel<T>.
//
TEST(ChannelTest, FixedChannel)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::FixedChannel<int, 4> ch(ctx);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < 4; i++) ch.Send(i);
            ch.Shutdown();
        });

        int sum = 0;
        int v;
        while (ch.Recv(v)) sum += v;

        EXPECT_EQ(sum, 6);  // 0+1+2+3
    });
}

// Channel<void> — counting channel; send/recv transfer no value.
//
TEST(ChannelTest, VoidChannel)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::FixedChannel<void, 4> ch(ctx);

        int count = 0;

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < 4; i++) ch.Send();
            ch.Shutdown();
        });

        while (ch.Recv()) count++;

        EXPECT_EQ(count, 4);
    });
}

// Select across a void channel and a typed channel in the same Select call.
//
TEST(ChannelTest, VoidChannelSelect)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::FixedChannel<void, 4> sig(ctx);
        coop::FixedChannel<int,  4> data(ctx);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            sig.Send();
            data.Send(42);
            sig.Shutdown();
            data.Shutdown();
        });

        int signals = 0, dataVal = -1;
        bool sigDone = false, dataDone = false;

        auto onSig  = coop::On(sig,  [&]()      { signals++; },   [&]{ sigDone  = true; });
        auto onData = coop::On(data, [&](int v) { dataVal = v; }, [&]{ dataDone = true; });

        while (!sigDone || !dataDone)
        {
            if      (!sigDone && !dataDone) coop::Select(ctx, onSig, onData);
            else if (!sigDone)              coop::Select(ctx, onSig);
            else                            coop::Select(ctx, onData);
        }

        EXPECT_EQ(signals,  1);
        EXPECT_EQ(dataVal, 42);
    });
}

// SelectAnyVoid — receive from whichever of N void channels fires.
//
TEST(ChannelTest, SelectAnyVoid)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::FixedChannel<void, 4> ch1(ctx);
        coop::FixedChannel<void, 4> ch2(ctx);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < 4; i++) ch1.Send();
            ch1.Shutdown();
        });

        int count = 0;
        while (coop::SelectAnyVoid(ch1, ch2)) count++;

        EXPECT_EQ(count, 4);
        ch2.Shutdown();
    });
}

// SelectAny — receive from whichever of N same-typed channels fires, without
// caring which one it was. Loop exits when the fired channel shuts down.
//
TEST(ChannelTest, SelectAny)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf1[4], buf2[4];
        coop::Channel<int> ch1(ctx, buf1, 4);
        coop::Channel<int> ch2(ctx, buf2, 4);

        // ch1 sends 4 items and shuts down; ch2 has no items.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 1; i <= 4; i++) ch1.Send(i);
            ch1.Shutdown();
        });

        int v, sum = 0, count = 0;
        while (coop::SelectAny(&v, ch1, ch2))
        {
            sum += v;
            count++;
        }

        EXPECT_EQ(count, 4);
        EXPECT_EQ(sum, 10);  // 1+2+3+4

        ch2.Shutdown();
    });
}
