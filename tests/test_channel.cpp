#include <gtest/gtest.h>

#include <memory>

#include "coop/channel.h"
#include "coop/select.h"
#include "coop/self.h"
#include "coop/ticker.h"
#include "test_helpers.h"

TEST(ChannelTest, SendRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[4];
        coop::chan::Channel<int> ch(ctx, buffer, 4);

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
        coop::chan::Channel<int> ch(ctx, buffer, 2);

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
        coop::chan::Channel<int> ch(ctx, buffer, 4);

        int value = 0;
        EXPECT_FALSE(ch.TryRecv(value));
    });
}

TEST(ChannelTest, Shutdown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buffer[4];
        coop::chan::Channel<int> ch(ctx, buffer, 4);

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
        coop::chan::Channel<int> ch(ctx, buffer, 1);

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
        coop::chan::Channel<int> ch(ctx, buffer, 4);

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
        coop::chan::Channel<int> ch(ctx, buffer, 4);

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
        coop::chan::Channel<int> ch(ctx, buffer, 1);

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
        coop::chan::Channel<int> ch(ctx, buf, 4);

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
        coop::chan::Channel<int> ch(ctx, buf, 2);

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
        coop::chan::Channel<int> ch(ctx, buf, 4);

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
        coop::chan::Channel<int> ch(ctx, buf, 4);

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
        coop::chan::Channel<int> ch(ctx, buffer, 8);

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
        coop::chan::Channel<int> ch1(ctx, buf1, 4);
        coop::chan::Channel<int> ch2(ctx, buf2, 4);

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

        auto on1 = coop::chan::On(ch1, [&](int v) { sum += v; count++; }, [&]{ done1 = true; });
        auto on2 = coop::chan::On(ch2, [&](int v) { sum += v; count++; }, [&]{ done2 = true; });

        while (!done1 || !done2)
        {
            if      (!done1 && !done2) coop::chan::Select(ctx, on1, on2);
            else if (!done1)           coop::chan::Select(ctx, on1);
            else                       coop::chan::Select(ctx, on2);
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
        coop::chan::Channel<int> ch1(ctx, buf1, 4);
        coop::chan::Channel<int> ch2(ctx, buf2, 4);

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
        while (coop::chan::Select(ctx,
            coop::chan::On(ch1, [&](int v) { received = v; }),
            coop::chan::On(ch2, [&](int v) { (void)v; })
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
        coop::chan::FixedChannel<int, 4> ch(ctx);

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

// Move-only types: send and receive unique_ptr through a channel.
//
TEST(ChannelTest, MoveOnlyType)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<std::unique_ptr<int>, 4> ch(ctx);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch.Send(std::make_unique<int>(42));
            ch.Send(std::make_unique<int>(100));
            ch.Shutdown();
        });

        std::unique_ptr<int> v;

        EXPECT_TRUE(ch.Recv(v));
        EXPECT_EQ(*v, 42);

        EXPECT_TRUE(ch.Recv(v));
        EXPECT_EQ(*v, 100);

        EXPECT_FALSE(ch.Recv(v));  // shutdown
    });
}

// Move-only type through blocking Send (channel full when sender starts).
//
TEST(ChannelTest, MoveOnlyTypeBlocking)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<std::unique_ptr<int>, 1> ch(ctx);

        // Fill the channel.
        //
        ch.TrySend(std::make_unique<int>(1));

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            // Blocks until the receiver drains, then sends via the blocking path.
            //
            ch.Send(std::make_unique<int>(2));
            ch.Shutdown();
        });

        std::unique_ptr<int> v;

        EXPECT_TRUE(ch.Recv(v));
        EXPECT_EQ(*v, 1);

        EXPECT_TRUE(ch.Recv(v));
        EXPECT_EQ(*v, 2);

        EXPECT_FALSE(ch.Recv(v));  // shutdown
    });
}

// Move-only type through Select (RecvCase::Fire default-constructs T then moves via RecvAcquired).
//
TEST(ChannelTest, MoveOnlyTypeSelect)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<std::unique_ptr<int>, 4> ch(ctx);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch.Send(std::make_unique<int>(42));
            ch.Shutdown();
        });

        int received = -1;
        bool ok = coop::chan::Select(ctx,
            coop::chan::On(ch, [&](std::unique_ptr<int> v){ received = *v; })
        );

        EXPECT_TRUE(ok);
        EXPECT_EQ(received, 42);
    });
}

// Channel<void> — counting channel; send/recv transfer no value.
//
TEST(ChannelTest, VoidChannel)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<void, 4> ch(ctx);

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
        coop::chan::FixedChannel<void, 4> sig(ctx);
        coop::chan::FixedChannel<int,  4> data(ctx);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            sig.Send();
            data.Send(42);
            sig.Shutdown();
            data.Shutdown();
        });

        int signals = 0, dataVal = -1;
        bool sigDone = false, dataDone = false;

        auto onSig  = coop::chan::On(sig,  [&]()      { signals++; },   [&]{ sigDone  = true; });
        auto onData = coop::chan::On(data, [&](int v) { dataVal = v; }, [&]{ dataDone = true; });

        while (!sigDone || !dataDone)
        {
            if      (!sigDone && !dataDone) coop::chan::Select(ctx, onSig, onData);
            else if (!sigDone)              coop::chan::Select(ctx, onSig);
            else                            coop::chan::Select(ctx, onData);
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
        coop::chan::FixedChannel<void, 4> ch1(ctx);
        coop::chan::FixedChannel<void, 4> ch2(ctx);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < 4; i++) ch1.Send();
            ch1.Shutdown();
        });

        int count = 0;
        while (coop::chan::SelectAnyVoid(ch1, ch2)) count++;

        EXPECT_EQ(count, 4);
        ch2.Shutdown();
    });
}

// Send-select: block until one of N channels has space, send to whichever fires first.
//
TEST(ChannelTest, SelectSend)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf1[1], buf2[1];
        coop::chan::Channel<int> ch1(ctx, buf1, 1);
        coop::chan::Channel<int> ch2(ctx, buf2, 1);

        // Fill ch1 so only ch2 has space.
        //
        EXPECT_TRUE(ch1.TrySend(0));

        int sent = -1;
        coop::chan::Select(ctx,
            coop::chan::OnSend(ch1, 10, [&]{ sent = 1; }),
            coop::chan::OnSend(ch2, 20, [&]{ sent = 2; })
        );

        EXPECT_EQ(sent, 2);

        int v;
        EXPECT_TRUE(ch2.TryRecv(v));
        EXPECT_EQ(v, 20);

        ch1.Shutdown();
        ch2.Shutdown();
    });
}

// Mixed recv+send select: block on whichever operation becomes possible first.
// The recv case fires because a spawned sender delivers to the recv channel.
//
TEST(ChannelTest, SelectMixedRecvSend)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf1[4], buf2[1];
        coop::chan::Channel<int> recvCh(ctx, buf1, 4);
        coop::chan::Channel<int> sendCh(ctx, buf2, 1);

        // Fill sendCh so its send case must wait for a receiver to drain it.
        //
        EXPECT_TRUE(sendCh.TrySend(0));

        // Spawned context will unblock the recv case.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            recvCh.Send(42);
        });

        int received = -1;
        bool gotRecv = false, gotSend = false;

        coop::chan::Select(ctx,
            coop::chan::On(recvCh,     [&](int v){ received = v; gotRecv = true; }),
            coop::chan::OnSend(sendCh, 99, [&]{ gotSend = true; })
        );

        EXPECT_TRUE(gotRecv);
        EXPECT_FALSE(gotSend);
        EXPECT_EQ(received, 42);

        recvCh.Shutdown();
        sendCh.Shutdown();
    });
}

// Send-select: shutdown on the target channel fires the shutdown callback and
// Select returns false.
//
TEST(ChannelTest, SelectSendShutdown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[1];
        coop::chan::Channel<int> ch(ctx, buf, 1);

        // Fill the channel so the send case will block.
        //
        EXPECT_TRUE(ch.TrySend(0));

        bool shutdownSeen = false;

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch.Shutdown();
        });

        bool ok = coop::chan::Select(ctx,
            coop::chan::OnSend(ch, 42, [&]{}, [&]{ shutdownSeen = true; })
        );

        EXPECT_FALSE(ok);
        EXPECT_TRUE(shutdownSeen);
    });
}

// Default fires immediately when no channel is ready; fires no case when data is available.
//
TEST(ChannelTest, SelectDefault)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[4];
        coop::chan::Channel<int> ch(ctx, buf, 4);

        bool defaultFired = false;
        int received = -1;

        // Channel empty — default fires.
        //
        bool ok = coop::chan::Select(ctx,
            coop::chan::On(ch, [&](int v){ received = v; }),
            coop::chan::Default([&]{ defaultFired = true; })
        );

        EXPECT_FALSE(ok);
        EXPECT_TRUE(defaultFired);
        EXPECT_EQ(received, -1);

        // Channel has data — recv case fires, default does not.
        //
        ch.TrySend(42);
        defaultFired = false;

        ok = coop::chan::Select(ctx,
            coop::chan::On(ch, [&](int v){ received = v; }),
            coop::chan::Default([&]{ defaultFired = true; })
        );

        EXPECT_TRUE(ok);
        EXPECT_FALSE(defaultFired);
        EXPECT_EQ(received, 42);

        ch.Shutdown();
    });
}

// Timeout case fires when no channel delivers within the deadline.
//
TEST(ChannelTest, SelectTimeout)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[4];
        coop::chan::Channel<int> ch(ctx, buf, 4);

        bool timedOut = false;
        int received = -1;

        // Nothing will send — timeout should fire.
        //
        bool ok = coop::chan::Select(ctx,
            coop::chan::On(ch, [&](int v) { received = v; }),
            coop::chan::Timeout(std::chrono::milliseconds(50), [&]{ timedOut = true; })
        );

        EXPECT_FALSE(ok);
        EXPECT_TRUE(timedOut);
        EXPECT_EQ(received, -1);

        ch.Shutdown();
    });
}

// Timeout does not fire when a channel delivers before the deadline.
//
TEST(ChannelTest, SelectTimeoutNotFired)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[4];
        coop::chan::Channel<int> ch(ctx, buf, 4);

        bool timedOut = false;

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch.Send(99);
        });

        bool ok = coop::chan::Select(ctx,
            coop::chan::On(ch, [&](int v) { EXPECT_EQ(v, 99); }),
            coop::chan::Timeout(std::chrono::milliseconds(5000), [&]{ timedOut = true; })
        );

        EXPECT_TRUE(ok);
        EXPECT_FALSE(timedOut);

        ch.Shutdown();
    });
}

// Timeout works with mixed recv+send cases.
//
TEST(ChannelTest, SelectTimeoutMixed)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int recvBuf[4], sendBuf[1];
        coop::chan::Channel<int> recvCh(ctx, recvBuf, 4);
        coop::chan::Channel<int> sendCh(ctx, sendBuf, 1);

        // Fill sendCh so its send case must wait.
        //
        EXPECT_TRUE(sendCh.TrySend(0));

        bool timedOut = false;

        bool ok = coop::chan::Select(ctx,
            coop::chan::On(recvCh, [&](int) {}),
            coop::chan::OnSend(sendCh, 42),
            coop::chan::Timeout(std::chrono::milliseconds(50), [&]{ timedOut = true; })
        );

        EXPECT_FALSE(ok);
        EXPECT_TRUE(timedOut);

        recvCh.Shutdown();
        sendCh.Shutdown();
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
        coop::chan::Channel<int> ch1(ctx, buf1, 4);
        coop::chan::Channel<int> ch2(ctx, buf2, 4);

        // ch1 sends 4 items and shuts down; ch2 has no items.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 1; i <= 4; i++) ch1.Send(i);
            ch1.Shutdown();
        });

        int v, sum = 0, count = 0;
        while (coop::chan::SelectAny(&v, ch1, ch2))
        {
            sum += v;
            count++;
        }

        EXPECT_EQ(count, 4);
        EXPECT_EQ(sum, 10);  // 1+2+3+4

        ch2.Shutdown();
    });
}

// Ticker fires at least N times before Stop() is called explicitly.
//
TEST(ChannelTest, TickerRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Ticker ticker(ctx, std::chrono::milliseconds(5));

        int ticks = 0;
        for (int i = 0; i < 3; i++)
        {
            EXPECT_TRUE(ticker.Chan().Recv());
            ticks++;
        }

        ticker.Stop();
        EXPECT_EQ(ticks, 3);
    });
}

// Ticker composes with SelectWithKill; Stop() after counting enough ticks.
//
TEST(ChannelTest, TickerSelect)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Ticker ticker(ctx, std::chrono::milliseconds(5));

        int ticks = 0;
        while (coop::chan::SelectWithKill(ctx,
            coop::chan::On(ticker.Chan(), [&]{ ticks++; })
        ) && ticks < 3) {}

        ticker.Stop();
        EXPECT_GE(ticks, 3);
    });
}

// Stop() immediately after construction must not hang.
//
TEST(ChannelTest, TickerStopImmediate)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Ticker ticker(ctx, std::chrono::milliseconds(5));
        ticker.Stop();
        // If we reach here without hanging, the test passes.
    });
}

// ~Ticker() calls Stop() automatically; no explicit Stop() needed.
//
TEST(ChannelTest, TickerDestroyWithoutStop)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        {
            coop::chan::Ticker ticker(ctx, std::chrono::milliseconds(5));
            // Let one tick arrive then let the destructor stop it.
            ticker.Chan().Recv();
        }
        // If we reach here without hanging, the test passes.
    });
}
