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
