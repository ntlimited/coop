#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

#include "coop/chan/channel.h"
#include "coop/chan/select.h"
#include "coop/self.h"
#include "coop/chan/ticker.h"
#include "coop/chan/pipe.h"
#include "coop/chan/merge.h"
#include "coop/chan/filter.h"
#include "coop/chan/passage.h"
#include "coop/chan/subscribe.h"
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

// ---------------------------------------------------------------------------
// Pipe tests
// ---------------------------------------------------------------------------

// Pipe transforms every item from the source channel.
//
TEST(ChannelTest, PipeBasicTransform)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int N = 10;

        int buf[4];
        coop::chan::Channel<int> src(ctx, buf, 4);

        auto pipe = coop::chan::Pipe(ctx, src, [](int v) { return v * 2; });

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++)
                src.Send(i);
            src.Shutdown();
        });

        int received = 0;
        int value = 0;
        while (pipe.Chan().Recv(value))
        {
            EXPECT_EQ(value, received * 2);
            received++;
        }

        EXPECT_EQ(received, N);
    });
}

// Source shutdown propagates: when the source shuts down, Recv on the pipe output
// returns false after all transformed items have been consumed.
//
TEST(ChannelTest, PipeSourceShutdownPropagates)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int N = 5;

        int buf[4];
        coop::chan::Channel<int> src(ctx, buf, 4);

        auto pipe = coop::chan::Pipe(ctx, src, [](int v) { return v + 100; });

        // Send N items then shut down
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++)
                src.Send(i);
            src.Shutdown();
        });

        int count = 0;
        int value = 0;
        while (pipe.Chan().Recv(value))
            count++;

        EXPECT_EQ(count, N);
        // After the loop, the pipe output channel is shut down.
    });
}

// Pipe composes: a PipeHandle implicitly converts to Channel<Out>& so it can be passed
// directly as the source to another Pipe stage.
//
TEST(ChannelTest, PipeChained)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int N = 8;

        int buf[4];
        coop::chan::Channel<int> src(ctx, buf, 4);

        // Stage 1: int → int (*2)
        //
        auto stage1 = coop::chan::Pipe(ctx, src, [](int v) { return v * 2; });

        // Stage 2: int → std::string (via implicit Channel<int>& conversion of stage1)
        //
        coop::chan::Channel<int>& stage1Ch = stage1;
        auto stage2 = coop::chan::Pipe(ctx, stage1Ch,
            [](int v) { return std::to_string(v); });

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++)
                src.Send(i);
            src.Shutdown();
        });

        int count = 0;
        std::string s;
        while (stage2.Chan().Recv(s))
        {
            EXPECT_EQ(s, std::to_string(count * 2));
            count++;
        }

        EXPECT_EQ(count, N);
    });
}

// Stop() before the source shuts down: the pipe output channel shuts down, and
// subsequent Recv returns false.
//
TEST(ChannelTest, PipeStopEarly)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Large buffer so the producer never blocks waiting for the pipe.
        //
        int buf[64];
        coop::chan::Channel<int> src(ctx, buf, 64);

        auto pipe = coop::chan::Pipe(ctx, src, [](int v) { return v; });

        // Producer runs indefinitely until killed.
        //
        coop::Context::Handle producerHandle;
        ctx->GetCooperator()->Spawn([&](coop::Context* producer)
        {
            int i = 0;
            while (src.Send(i++)) {}
        }, &producerHandle);

        // Consume a couple of items then stop the pipe.
        //
        int value = 0;
        EXPECT_TRUE(pipe.Chan().Recv(value));
        EXPECT_TRUE(pipe.Chan().Recv(value));

        pipe.Stop();

        // After Stop(), Recv must return false (channel is shut down).
        //
        EXPECT_FALSE(pipe.Chan().Recv(value));

        // Clean up the producer.
        //
        src.Shutdown();
        producerHandle.Kill();
    });
}

// PipeHandle composes with Select via its implicit Channel<Out>& conversion.
//
TEST(ChannelTest, PipeSelectComposition)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int N = 5;

        int buf[4];
        coop::chan::Channel<int> src(ctx, buf, 4);

        auto pipe = coop::chan::Pipe(ctx, src, [](int v) { return v * 3; });

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++)
                src.Send(i);
            src.Shutdown();
        });

        int count = 0;
        int value = 0;
        while (coop::chan::SelectWithKill(ctx,
            coop::chan::On(pipe.Chan(), [&](int v)
            {
                EXPECT_EQ(v, count * 3);
                count++;
            })
        )) {}

        EXPECT_EQ(count, N);
    });
}

// ---------------------------------------------------------------------------
// Merge tests
// ---------------------------------------------------------------------------

// Items from both sources arrive interleaved in availability order.
//
TEST(ChannelTest, MergeBasic)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int N = 8;

        int bufA[4], bufB[4];
        coop::chan::Channel<int> chA(ctx, bufA, 4);
        coop::chan::Channel<int> chB(ctx, bufB, 4);

        auto merged = coop::chan::Merge(ctx, chA, chB);

        // Producer A: even numbers
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i += 2)
                chA.Send(i);
            chA.Shutdown();
        });

        // Producer B: odd numbers
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 1; i < N; i += 2)
                chB.Send(i);
            chB.Shutdown();
        });

        int count = 0;
        int value = 0;
        while (merged.Chan().Recv(value))
            count++;

        EXPECT_EQ(count, N);
    });
}

// When one source shuts down first, the merge continues draining the other.
//
TEST(ChannelTest, MergeOneSourceDiesEarly)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int NA = 3;
        constexpr int NB = 7;

        int bufA[4], bufB[4];
        coop::chan::Channel<int> chA(ctx, bufA, 4);
        coop::chan::Channel<int> chB(ctx, bufB, 4);

        auto merged = coop::chan::Merge(ctx, chA, chB);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < NA; i++) chA.Send(i);
            chA.Shutdown();
        });

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < NB; i++) chB.Send(100 + i);
            chB.Shutdown();
        });

        int count = 0;
        int value = 0;
        while (merged.Chan().Recv(value))
            count++;

        EXPECT_EQ(count, NA + NB);
    });
}

// Merge composes with Pipe: chain a 2-stage pipeline on each source then merge.
//
TEST(ChannelTest, MergePipeComposition)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int N = 5;

        int bufA[4], bufB[4];
        coop::chan::Channel<int> chA(ctx, bufA, 4);
        coop::chan::Channel<int> chB(ctx, bufB, 4);

        // Pipe each source through a transform, then merge.
        //
        auto pipedA = coop::chan::Pipe(ctx, chA, [](int v) { return v * 2; });
        auto pipedB = coop::chan::Pipe(ctx, chB, [](int v) { return v * 3; });

        coop::chan::Channel<int>& pA = pipedA;
        coop::chan::Channel<int>& pB = pipedB;
        auto merged = coop::chan::Merge(ctx, pA, pB);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++) chA.Send(i);
            chA.Shutdown();
        });

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++) chB.Send(i);
            chB.Shutdown();
        });

        int sum = 0;
        int value = 0;
        int count = 0;
        while (merged.Chan().Recv(value))
        {
            sum += value;
            count++;
        }

        // A produces: 0,2,4,6,8 (sum=20); B produces: 0,3,6,9,12 (sum=30)
        //
        EXPECT_EQ(count, 2 * N);
        EXPECT_EQ(sum, 50);
    });
}

// Stop() before sources shut down: output channel shuts down, Recv returns false.
//
TEST(ChannelTest, MergeStopEarly)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int bufA[64], bufB[64];
        coop::chan::Channel<int> chA(ctx, bufA, 64);
        coop::chan::Channel<int> chB(ctx, bufB, 64);

        auto merged = coop::chan::Merge(ctx, chA, chB);

        coop::Context::Handle hA, hB;
        ctx->GetCooperator()->Spawn([&](coop::Context*) { int i = 0; while (chA.Send(i++)) {} }, &hA);
        ctx->GetCooperator()->Spawn([&](coop::Context*) { int i = 0; while (chB.Send(i++)) {} }, &hB);

        // Consume a couple of items then stop.
        //
        int value = 0;
        EXPECT_TRUE(merged.Chan().Recv(value));
        EXPECT_TRUE(merged.Chan().Recv(value));

        merged.Stop();

        // Channel drains buffered items before returning false on shutdown;
        // loop to completion then verify closed.
        //
        while (merged.Chan().Recv(value)) {}
        EXPECT_FALSE(merged.Chan().Recv(value));

        chA.Shutdown();
        chB.Shutdown();
        hA.Kill();
        hB.Kill();
    });
}

// Chained merge: 3 sources combined as Merge(Merge(a,b), c).
//
TEST(ChannelTest, MergeChained)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int N = 4;

        int bA[4], bB[4], bC[4];
        coop::chan::Channel<int> chA(ctx, bA, 4);
        coop::chan::Channel<int> chB(ctx, bB, 4);
        coop::chan::Channel<int> chC(ctx, bC, 4);

        auto m1 = coop::chan::Merge(ctx, chA, chB);
        coop::chan::Channel<int>& m1ch = m1;
        auto m2 = coop::chan::Merge(ctx, m1ch, chC);

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++) chA.Send(i);
            chA.Shutdown();
        });
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++) chB.Send(i);
            chB.Shutdown();
        });
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++) chC.Send(i);
            chC.Shutdown();
        });

        int count = 0;
        int value = 0;
        while (m2.Chan().Recv(value))
            count++;

        EXPECT_EQ(count, 3 * N);
    });
}

// ---------------------------------------------------------------------------
// Filter tests
// ---------------------------------------------------------------------------

// Filter passes only items matching the predicate.
//
TEST(ChannelTest, FilterBasic)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int N = 10;

        int buf[4];
        coop::chan::Channel<int> src(ctx, buf, 4);

        auto evens = coop::chan::Filter(ctx, src, [](int v) { return v % 2 == 0; });

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < N; i++)
                src.Send(i);
            src.Shutdown();
        });

        int count = 0;
        int value = 0;
        while (evens.Chan().Recv(value))
        {
            EXPECT_EQ(value % 2, 0);
            count++;
        }

        EXPECT_EQ(count, N / 2);
    });
}

// Filter that passes nothing still propagates shutdown.
//
TEST(ChannelTest, FilterRejectAll)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[4];
        coop::chan::Channel<int> src(ctx, buf, 4);

        auto none = coop::chan::Filter(ctx, src, [](int) { return false; });

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < 5; i++)
                src.Send(i);
            src.Shutdown();
        });

        int value = 0;
        EXPECT_FALSE(none.Chan().Recv(value));
    });
}

// Filter composes with Pipe: pipe → filter → consumer.
//
TEST(ChannelTest, FilterPipeComposition)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[4];
        coop::chan::Channel<int> src(ctx, buf, 4);

        auto doubled = coop::chan::Pipe(ctx, src, [](int v) { return v * 2; });
        auto big     = coop::chan::Filter(ctx, doubled.Chan(),
                                          [](int v) { return v > 10; });

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < 10; i++)
                src.Send(i);
            src.Shutdown();
        });

        // doubled: 0,2,4,6,8,10,12,14,16,18 → big keeps > 10: 12,14,16,18
        //
        int count = 0;
        int value = 0;
        while (big.Chan().Recv(value))
        {
            EXPECT_GT(value, 10);
            count++;
        }

        EXPECT_EQ(count, 4);
    });
}

// Stop() shuts down early, same as Pipe.
//
TEST(ChannelTest, FilterStopEarly)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int buf[64];
        coop::chan::Channel<int> src(ctx, buf, 64);

        // Pass everything so no items are dropped.
        //
        auto pass = coop::chan::Filter(ctx, src, [](int) { return true; });

        coop::Context::Handle hProd;
        ctx->GetCooperator()->Spawn(
            [&](coop::Context*) { int i = 0; while (src.Send(i++)) {} }, &hProd);

        int value = 0;
        EXPECT_TRUE(pass.Chan().Recv(value));

        pass.Stop();
        while (pass.Chan().Recv(value)) {}
        EXPECT_FALSE(pass.Chan().Recv(value));

        src.Shutdown();
        hProd.Kill();
    });
}

// ---------------------------------------------------------------------------
// Passage tests
// ---------------------------------------------------------------------------

// External thread sends N items; cooperator consumer receives them all.
//
TEST(PassageTest, BasicSendRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Passage<int> passage(ctx, ctx->GetCooperator());

        constexpr int N = 10;
        std::thread sender([&]
        {
            for (int i = 0; i < N; i++)
                EXPECT_TRUE(passage.Send(i));
        });

        int sum = 0;
        for (int i = 0; i < N; i++)
        {
            int v;
            ASSERT_TRUE(passage.Recv(v));
            sum += v;
        }

        sender.join();
        EXPECT_EQ(sum, 45);  // 0+1+...+9
    });
}

// Multiple external threads send concurrently; all items are delivered.
//
TEST(PassageTest, MultipleProducers)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int NTHREADS = 4;
        constexpr int PER_THREAD = 16;
        constexpr int TOTAL = NTHREADS * PER_THREAD;

        // Use a large enough passage so no thread drops items.
        //
        coop::chan::Passage<int, TOTAL> passage(ctx, ctx->GetCooperator());

        std::thread senders[NTHREADS];
        for (int t = 0; t < NTHREADS; t++)
        {
            senders[t] = std::thread([&, t]
            {
                for (int i = 0; i < PER_THREAD; i++)
                    while (!passage.Send(t * PER_THREAD + i)) {}
            });
        }

        int received = 0;
        while (received < TOTAL)
        {
            int v;
            ASSERT_TRUE(passage.Recv(v));
            received++;
        }

        for (auto& t : senders) t.join();
        EXPECT_EQ(received, TOTAL);
    });
}

// External ring capacity limit: Send() returns false when the ring is full.
//
TEST(PassageTest, BackpressureRingFull)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Passage<int, 2> passage(ctx, ctx->GetCooperator());

        // Fill ring from the cooperator thread itself (no external thread needed).
        //
        EXPECT_TRUE(passage.Send(1));
        EXPECT_TRUE(passage.Send(2));
        EXPECT_FALSE(passage.Send(3));  // ring full — back-pressure

        // Drain so the consumer loop terminates cleanly.
        //
        int v;
        EXPECT_TRUE(passage.Recv(v)); EXPECT_EQ(v, 1);
        EXPECT_TRUE(passage.Recv(v)); EXPECT_EQ(v, 2);
    });
}

// TryRecv and Drain provide non-blocking single and batch recv APIs.
//
TEST(PassageTest, TryRecvAndDrain)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Passage<int, 8> passage(ctx, ctx->GetCooperator());

        for (int i = 0; i < 5; i++)
            EXPECT_TRUE(passage.Send(i));

        int v = -1;
        EXPECT_TRUE(passage.TryRecv(v));
        EXPECT_EQ(v, 0);

        int out[8]{};
        size_t n = passage.Drain(out, 8);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(out[0], 1);
        EXPECT_EQ(out[1], 2);
        EXPECT_EQ(out[2], 3);
        EXPECT_EQ(out[3], 4);

        EXPECT_FALSE(passage.TryRecv(v));
        EXPECT_EQ(passage.Drain(out, 8), 0u);
    });
}

// Recv tuning parameters should be configurable at construction.
//
TEST(PassageTest, CustomRecvTuning)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Passage<int>::RecvTuning tuning;
        tuning.yieldThreshold = 0;
        tuning.timeoutInitialUs = 5;
        tuning.timeoutMaxUs = 20;
        tuning.timeoutBackoff = 2;

        coop::chan::Passage<int> passage(ctx, ctx->GetCooperator(), tuning);

        std::thread sender([&]
        {
            while (!passage.Send(7)) {}
        });

        int v = 0;
        ASSERT_TRUE(passage.Recv(v));
        EXPECT_EQ(v, 7);
        sender.join();
    });
}

// After Shutdown(), Recv() returns false once the ring is drained.
//
TEST(PassageTest, ShutdownDrainsInternalChannel)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Passage<int> passage(ctx, ctx->GetCooperator());

        constexpr int N = 5;
        std::thread sender([&]
        {
            for (int i = 0; i < N; i++)
                while (!passage.Send(i)) {}
        });

        // Receive all N items.
        //
        for (int i = 0; i < N; i++)
        {
            int v;
            ASSERT_TRUE(passage.Recv(v));
        }

        sender.join();
        passage.Shutdown();

        // Ring is drained and shut down — next Recv returns false.
        //
        int v;
        EXPECT_FALSE(passage.Recv(v));
    });
}

// Shutdown is thread-safe and should wake a blocked receiver when called from
// an external thread.
//
TEST(PassageTest, ShutdownFromExternalThread)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Passage<int> passage(ctx, ctx->GetCooperator());

        std::thread killer([&]
        {
            passage.Shutdown();
        });

        int v = 0;
        EXPECT_FALSE(passage.Recv(v));
        killer.join();
    });
}

// Send racing with Shutdown may either enqueue or fail. If enqueued, the value
// is still delivered before Recv() returns false.
//
TEST(PassageTest, SendRaceWithShutdown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Passage<int> passage(ctx, ctx->GetCooperator());

        std::atomic<bool> go{false};
        std::atomic<int> sendOk{-1};

        std::thread sender([&]
        {
            while (!go.load(std::memory_order_acquire)) {}
            sendOk.store(passage.Send(42) ? 1 : 0, std::memory_order_release);
        });

        go.store(true, std::memory_order_release);
        passage.Shutdown();
        sender.join();

        int v = 0;
        if (sendOk.load(std::memory_order_acquire) == 1)
        {
            ASSERT_TRUE(passage.Recv(v));
            EXPECT_EQ(v, 42);
        }
        EXPECT_FALSE(passage.Recv(v));
    });
}

// Target cooperator shutdown should make Send fail fast.
//
TEST(PassageTest, SendFailsWhenTargetShuttingDown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ctx->GetCooperator()->Shutdown();
        coop::chan::Passage<int> passage(ctx, ctx->GetCooperator());

        EXPECT_FALSE(passage.Send(1));
        int v = 0;
        EXPECT_FALSE(passage.Recv(v));
    });
}

// Passage::Recv() — adaptive yield → timed-wait path
//
TEST(PassageTest, AdaptiveRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int N = 20;
        coop::chan::Passage<int> passage(ctx, ctx->GetCooperator());

        std::thread sender([&]
        {
            for (int i = 0; i < N; i++)
                while (!passage.Send(i)) {}
        });

        int sum = 0;
        for (int i = 0; i < N; i++)
        {
            int v;
            ASSERT_TRUE(passage.Recv(v));
            sum += v;
        }

        sender.join();
        EXPECT_EQ(sum, N * (N - 1) / 2);  // 0..19
    });
}

// Shutdown while Passage::Recv() is in the timed-wait phase should return false.
//
TEST(PassageTest, AdaptiveRecvShutdown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::Passage<int> passage(ctx, ctx->GetCooperator());

        // Spawn a context that shuts down the passage after a short cooperative delay.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < 20; i++)
                coop::Yield();
            passage.Shutdown();
        });

        // Recv should eventually return false once shutdown propagates.
        //
        int v;
        bool result = passage.Recv(v);
        EXPECT_FALSE(result);
    });
}

// ---------------------------------------------------------------------------
// SpscPassage tests
// ---------------------------------------------------------------------------

TEST(SpscPassageTest, BasicSendRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::SpscPassage<int> passage(ctx, ctx->GetCooperator());

        constexpr int N = 10;
        std::thread sender([&]
        {
            for (int i = 0; i < N; i++)
                EXPECT_TRUE(passage.Send(i));
        });

        int sum = 0;
        for (int i = 0; i < N; i++)
        {
            int v = 0;
            ASSERT_TRUE(passage.Recv(v));
            sum += v;
        }

        sender.join();
        EXPECT_EQ(sum, 45);
    });
}

TEST(SpscPassageTest, TryRecvAndDrain)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::SpscPassage<int, 8> passage(ctx, ctx->GetCooperator());

        for (int i = 0; i < 5; i++)
            EXPECT_TRUE(passage.Send(i));

        int v = -1;
        EXPECT_TRUE(passage.TryRecv(v));
        EXPECT_EQ(v, 0);

        int out[8]{};
        const size_t n = passage.Drain(out, 8);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(out[0], 1);
        EXPECT_EQ(out[1], 2);
        EXPECT_EQ(out[2], 3);
        EXPECT_EQ(out[3], 4);

        EXPECT_FALSE(passage.TryRecv(v));
        EXPECT_EQ(passage.Drain(out, 8), 0u);
    });
}

TEST(SpscPassageTest, ShutdownFromExternalThread)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::SpscPassage<int> passage(ctx, ctx->GetCooperator());

        std::thread killer([&]
        {
            passage.Shutdown();
        });

        int v = 0;
        EXPECT_FALSE(passage.Recv(v));
        killer.join();
    });
}

// ---------------------------------------------------------------------------
// Subscribe / Drain -- object-hosted, continuation-dispatched fan-in. The handlers are
// run-to-completion thunks; each Drain case re-arms IN PLACE (no per-fire heap). See
// coop/chan/subscribe.h for the contract.
//
// NOTE: a handler that suspends (Yield/Block/blocking Recv) inside the thunk body is a contract
// violation -- it trips AssertNotInThunk and aborts in debug. That is the debug-abort calibration
// case; it is deliberately NOT exercised here so the suite does not abort.
// ---------------------------------------------------------------------------

namespace
{

struct SubMsg
{
    int channel;
    int seq;
};

} // namespace

// Every item from both channels reaches its handler exactly once and in per-channel order, with no
// parked consumer context: the handlers are continuations fired from the scheduler loop's drain.
//
TEST(ChannelTest, SubscribeDeliversInOrder)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int PER = 5;

        coop::chan::FixedChannel<SubMsg, 4> ch0(ctx);
        coop::chan::FixedChannel<SubMsg, 4> ch1(ctx);

        int received[2] = {0, 0};
        int nextSeq[2]  = {0, 0};
        int outOfOrder  = 0;
        int total       = 0;

        auto sub = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch0, [&](SubMsg&& m)
            {
                if (m.seq != nextSeq[0]) outOfOrder++;
                nextSeq[0]++; received[0]++; total++;
            }),
            coop::chan::Drain(ch1, [&](SubMsg&& m)
            {
                if (m.seq != nextSeq[1]) outOfOrder++;
                nextSeq[1]++; received[1]++; total++;
            }));

        coop::chan::Channel<SubMsg>* chans[2] = {&ch0, &ch1};
        for (int c = 0; c < 2; c++)
        {
            ctx->GetCooperator()->Spawn([&, c](coop::Context*)
            {
                for (int i = 0; i < PER; i++) chans[c]->Send(SubMsg{c, i});
            });
        }

        while (total < 2 * PER) ctx->Yield(true);

        EXPECT_EQ(outOfOrder, 0);
        EXPECT_EQ(total, 2 * PER);
        EXPECT_EQ(received[0], PER);
        EXPECT_EQ(received[1], PER);

        // Shut both down so every arm retires before teardown (a still-armed node would trip the
        // ~Coordinator "waiters still queued" assert). Join on it.
        //
        ch0.Shutdown();
        ch1.Shutdown();
        sub.Wait();
    });
}

// Drain-all-per-fire: m_recv is edge-triggered, so a burst of sends into an already-non-empty
// channel pulses it only once. A single fire must drain every queued item.
//
TEST(ChannelTest, SubscribeDrainsBurstInOneFire)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int BURST = 6;

        coop::chan::FixedChannel<SubMsg, 8> ch(ctx);

        int received   = 0;
        int nextSeq    = 0;
        int outOfOrder = 0;

        auto sub = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch, [&](SubMsg&& m)
            {
                if (m.seq != nextSeq) outOfOrder++;
                nextSeq++; received++;
            }));

        // A producer dumps the whole burst without yielding: all BURST items land in the channel,
        // but only the first Send pulses m_recv (empty -> non-empty edge).
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            for (int i = 0; i < BURST; i++) ch.Send(SubMsg{0, i});
        });

        while (received < BURST) ctx->Yield(true);

        EXPECT_EQ(outOfOrder, 0);
        EXPECT_EQ(received, BURST);

        ch.Shutdown();
        sub.Wait();
    });
}

// Shutting a channel down retires its arm: the terminal fire drains the tail, observes IsShutdown,
// and does not re-arm. Wait() returns once that last (only) arm has retired.
//
TEST(ChannelTest, SubscribeShutdownRetiresArm)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<SubMsg, 4> ch(ctx);

        int received = 0;

        auto sub = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch, [&](SubMsg&&) { received++; }));

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch.Send(SubMsg{0, 0});
            ch.Send(SubMsg{0, 1});
        });

        while (received < 2) ctx->Yield(true);

        ch.Shutdown();
        sub.Wait();              // join returns once the shut-down arm retires
        EXPECT_EQ(received, 2);
    });
}

// Control::Stop() from inside a handler retires THAT arm (subsequent sends to it are not delivered)
// while the other arm keeps running.
//
TEST(ChannelTest, SubscribeControlStopRetiresOneArm)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<SubMsg, 4> chStop(ctx);
        coop::chan::FixedChannel<SubMsg, 4> chLive(ctx);

        int stopReceived = 0;
        int liveReceived = 0;

        auto sub = coop::chan::Subscribe(ctx,
            coop::chan::Drain(chStop, [&](SubMsg&&, coop::chan::Control& ctl)
            {
                stopReceived++;
                ctl.Stop();      // retire this arm after this item
            }),
            coop::chan::Drain(chLive, [&](SubMsg&&)
            {
                liveReceived++;
            }));

        // First item to chStop triggers Stop. Deliver it, let the arm retire.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            chStop.Send(SubMsg{0, 0});
        });
        while (stopReceived < 1) ctx->Yield(true);
        EXPECT_EQ(stopReceived, 1);

        // Subsequent sends to the stopped channel are NOT delivered; the live arm still is.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            chStop.Send(SubMsg{0, 1});      // stranded -- arm retired
            chLive.Send(SubMsg{1, 0});
            chLive.Send(SubMsg{1, 1});
        });
        while (liveReceived < 2) ctx->Yield(true);

        EXPECT_EQ(stopReceived, 1);         // never advanced past the Stop
        EXPECT_EQ(liveReceived, 2);

        chLive.Shutdown();                  // chStop's arm is already retired
        sub.Wait();
    });
}

// Control::CancelAll() from inside a handler retires the WHOLE subscription. After it, no arm
// delivers, and Wait() returns immediately (completion already latched).
//
TEST(ChannelTest, SubscribeControlCancelAllRetiresSubscription)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<SubMsg, 4> ch0(ctx);
        coop::chan::FixedChannel<SubMsg, 4> ch1(ctx);

        int r0 = 0;
        int r1 = 0;

        auto sub = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch0, [&](SubMsg&&, coop::chan::Control& ctl)
            {
                r0++;
                ctl.CancelAll();
            }),
            coop::chan::Drain(ch1, [&](SubMsg&&)
            {
                r1++;
            }));

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch0.Send(SubMsg{0, 0});
        });
        while (r0 < 1) ctx->Yield(true);
        EXPECT_EQ(r0, 1);

        // Whole subscription is retired now: further sends to EITHER channel are not delivered.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch0.Send(SubMsg{0, 1});
            ch1.Send(SubMsg{1, 0});
        });
        ctx->Yield(true);
        ctx->Yield(true);

        EXPECT_EQ(r0, 1);
        EXPECT_EQ(r1, 0);

        sub.Wait();                         // already complete -> returns at once
    });
}

// Wait() is a join: it returns once every channel has shut down (all arms retired), not before.
//
TEST(ChannelTest, SubscribeWaitJoinsOnAllShutdown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<SubMsg, 4> ch0(ctx);
        coop::chan::FixedChannel<SubMsg, 4> ch1(ctx);

        int total = 0;

        auto sub = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch0, [&](SubMsg&&) { total++; }),
            coop::chan::Drain(ch1, [&](SubMsg&&) { total++; }));

        bool joined = false;

        // A waiter context blocks in Wait() until BOTH channels are shut down.
        //
        coop::Context::Handle waiterHandle;
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            sub.Wait();
            joined = true;
        }, &waiterHandle);

        ctx->Yield(true);
        EXPECT_FALSE(joined);               // nothing shut down yet

        ch0.Shutdown();
        ctx->Yield(true);
        EXPECT_FALSE(joined);               // one arm still live

        ch1.Shutdown();
        ctx->Yield(true);
        ctx->Yield(true);
        EXPECT_TRUE(joined);                // both retired -> join completed
    });
}

// Courtesy trigger: a channel that already holds buffered data at Subscribe time has it adopted
// immediately (delivered synchronously during Subscribe), not stranded until the next Send. The arm
// is then correctly armed for future traffic.
//
TEST(ChannelTest, SubscribeAdoptsBufferedItems)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<SubMsg, 4> ch(ctx);

        // Pre-load items BEFORE subscribing. m_recv goes empty->non-empty on the first push and is
        // not re-pulsed by the rest -- exactly the edge case the courtesy fire exists for.
        //
        EXPECT_TRUE(ch.TrySend(SubMsg{0, 0}));
        EXPECT_TRUE(ch.TrySend(SubMsg{0, 1}));
        EXPECT_TRUE(ch.TrySend(SubMsg{0, 2}));

        int received   = 0;
        int nextSeq    = 0;
        int outOfOrder = 0;

        auto sub = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch, [&](SubMsg&& m)
            {
                if (m.seq != nextSeq) outOfOrder++;
                nextSeq++; received++;
            }));

        // Adopted synchronously by the courtesy fire during Subscribe -- no scheduler turn needed.
        //
        EXPECT_EQ(received, 3);
        EXPECT_EQ(outOfOrder, 0);

        // And the arm is left correctly armed: a subsequent Send re-pulses m_recv and fires it.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch.Send(SubMsg{0, 3});
        });
        while (received < 4) ctx->Yield(true);
        EXPECT_EQ(received, 4);
        EXPECT_EQ(outOfOrder, 0);

        ch.Shutdown();
        sub.Wait();
    });
}

// Subscribing to a channel that is ALREADY shut down and empty must retire the arm during
// ArmInitial rather than registering it: Shutdown already pulsed m_recv to an empty wait list, so a
// registered arm would never fire and Wait() would hang. The handler never runs.
//
TEST(ChannelTest, SubscribeAlreadyShutdownEmptyRetiresImmediately)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<SubMsg, 4> ch(ctx);
        ch.Shutdown();                       // shut down BEFORE subscribing, channel empty

        int received = 0;
        auto sub = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch, [&](SubMsg&&) { received++; }));

        sub.Wait();                          // must return immediately -- arm retired in ArmInitial
        EXPECT_EQ(received, 0);               // never fired
    });
}

// The buffered-but-already-shut-down variant: items still sit in the channel when we subscribe AND
// it is shut down. ArmInitial sees a non-empty channel, so its courtesy fire drains the buffered
// items (delivering them), then FireOnce observes IsShutdown and retires -- the data is not lost
// and Wait() still joins.
//
TEST(ChannelTest, SubscribeAlreadyShutdownDrainsBufferThenRetires)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<SubMsg, 4> ch(ctx);
        EXPECT_TRUE(ch.TrySend(SubMsg{0, 0}));
        EXPECT_TRUE(ch.TrySend(SubMsg{0, 1}));
        ch.Shutdown();                       // buffered items remain; channel shut down

        int received = 0;
        auto sub = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch, [&](SubMsg&&) { received++; }));

        EXPECT_EQ(received, 2);               // courtesy-drained synchronously before retiring
        sub.Wait();                           // joins -- arm retired after the drain
    });
}

// The subscription-level ops are reachable through the pure-virtual Subscription interface: a
// caller can collect heterogeneous subscriptions as Subscription* and Cancel/Wait them uniformly.
//
TEST(ChannelTest, SubscribeDrivenThroughBaseInterface)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::chan::FixedChannel<SubMsg, 4> ch0(ctx);
        coop::chan::FixedChannel<SubMsg, 4> ch1(ctx);

        int r = 0;

        // Two independent subscriptions with different (here identical) arm shapes.
        //
        auto subA = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch0, [&](SubMsg&&) { r++; }));
        auto subB = coop::chan::Subscribe(ctx,
            coop::chan::Drain(ch1, [&](SubMsg&&) { r++; }));

        coop::chan::Subscription* subs[2] = {&subA, &subB};

        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch0.Send(SubMsg{0, 0});
            ch1.Send(SubMsg{1, 0});
        });
        while (r < 2) ctx->Yield(true);
        EXPECT_EQ(r, 2);

        // Cancel both through the base interface (no knowledge of the concrete arm types).
        //
        for (auto* s : subs)
        {
            s->Cancel();
        }

        // After cancel, further sends are not delivered.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context*)
        {
            ch0.Send(SubMsg{0, 1});
            ch1.Send(SubMsg{1, 1});
        });
        ctx->Yield(true);
        ctx->Yield(true);
        EXPECT_EQ(r, 2);

        // Wait through a base reference -- already complete (cancelled), returns at once.
        //
        coop::chan::Subscription& base = subA;
        base.Wait();
    });
}
