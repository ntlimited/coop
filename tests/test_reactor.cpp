#include <gtest/gtest.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/io/reactor.h"
#include "coop/time/sleep.h"

#include "test_helpers.h"

namespace
{

struct State
{
    int      readyCount   = 0;
    unsigned lastRevents  = 0;
    int      timeoutCount = 0;
};

void OnReady(int fd, unsigned revents, void* user)
{
    auto* s = static_cast<State*>(user);
    s->readyCount++;
    s->lastRevents = revents;

    // Drain the fd non-blockingly, as any real readiness consumer must: the reactor re-arms one-shot
    // polls, so a callback that leaves the fd readable would just fire again (edge-triggered
    // contract). recv must never block the cooperator thread.
    //
    char buf[64];
    while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
}

void OnTimeout(void* user)
{
    static_cast<State*>(user)->timeoutCount++;
}

} // namespace

// A watched fd that becomes readable fires the readiness callback with POLLIN.
//
TEST(ReactorTest, ReadinessFires)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

        State st;
        coop::io::Reactor reactor(ctx->GetCooperator(), &OnReady, &OnTimeout, &st);

        reactor.Watch(fds[0], POLLIN);
        for (int i = 0; i < 4; i++) ctx->Yield(true);   // let the watcher arm its poll

        EXPECT_EQ(st.readyCount, 0);                     // nothing readable yet

        ASSERT_EQ(write(fds[1], "x", 1), 1);             // make fds[0] readable
        for (int i = 0; i < 6; i++) ctx->Yield(true);    // let the watcher fire

        EXPECT_GE(st.readyCount, 1);
        EXPECT_TRUE(st.lastRevents & POLLIN);

        // The watcher drained fds[0] and is now blocked on its poll. Unwatch it, then nudge the fd
        // readable so its poll returns and it observes the retirement and exits cleanly.
        //
        reactor.Unwatch(fds[0]);
        ASSERT_EQ(write(fds[1], "y", 1), 1);
        for (int i = 0; i < 6; i++) ctx->Yield(true);

        close(fds[0]);
        close(fds[1]);
    });
}

// An unwatched fd never fires, even when it becomes readable.
//
TEST(ReactorTest, UnwatchedFdSilent)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

        State st;
        coop::io::Reactor reactor(ctx->GetCooperator(), &OnReady, &OnTimeout, &st);

        // Never watched.
        ASSERT_EQ(write(fds[1], "x", 1), 1);
        for (int i = 0; i < 6; i++) ctx->Yield(true);

        EXPECT_EQ(st.readyCount, 0);

        close(fds[0]);
        close(fds[1]);
    });
}

// The single timeout fires after its interval; a superseding SetTimeout replaces the pending one.
//
TEST(ReactorTest, TimeoutFires)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        State st;
        coop::io::Reactor reactor(ctx->GetCooperator(), &OnReady, &OnTimeout, &st);

        reactor.SetTimeout(200);        // pending...
        reactor.SetTimeout(10);         // ...superseded by a sooner one

        coop::time::Sleep(ctx, std::chrono::milliseconds(60));

        EXPECT_EQ(st.timeoutCount, 1);  // exactly one fire — the superseded timer did not also fire
    });
}

// SetTimeout(-1) cancels a pending timeout.
//
TEST(ReactorTest, TimeoutCancel)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        State st;
        coop::io::Reactor reactor(ctx->GetCooperator(), &OnReady, &OnTimeout, &st);

        reactor.SetTimeout(10);
        reactor.SetTimeout(-1);         // cancel

        coop::time::Sleep(ctx, std::chrono::milliseconds(40));

        EXPECT_EQ(st.timeoutCount, 0);
    });
}
