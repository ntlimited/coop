#include <gtest/gtest.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <semaphore>
#include <thread>

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

// Destroying a Reactor with a timeout still parked is legal, non-blocking, and silent.
//
// This is the direct regression for a heap-use-after-free: the timer context is detached and holds
// the reactor's shared state, so before the shared_ptr covenant in reactor.cpp it woke after
// ~Reactor and read freed memory (reactor.cpp's timerGen). Two things are asserted at once —
// that waking after destruction is memory-safe (ASan is the real assertion here), and that the
// timeout does NOT fire, because the callback and `user` belong to an owner that is now gone.
//
TEST(ReactorTest, DestroyedWithTimerParked)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        State st;
        {
            coop::io::Reactor reactor(ctx->GetCooperator(), &OnReady, &OnTimeout, &st);
            reactor.SetTimeout(40);
        }                                                   // parked timer outlives the Reactor

        coop::time::Sleep(ctx, std::chrono::milliseconds(90));   // sleep well past its deadline

        EXPECT_EQ(st.timeoutCount, 0);                      // a dead Reactor never calls back
    });
}

// The same covenant for the watcher, which is the timer's sibling: also detached, also holding the
// reactor's shared state across a park (io::Poll rather than a sleep). Destroy the Reactor while a
// watcher is blocked, then make the fd readable so the watcher actually wakes and re-reads the
// state it captured — the point being that it wakes into live memory and declines to call back.
//
TEST(ReactorTest, DestroyedWithWatcherParked)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

        State st;
        {
            coop::io::Reactor reactor(ctx->GetCooperator(), &OnReady, &OnTimeout, &st);
            reactor.Watch(fds[0], POLLIN);
            for (int i = 0; i < 4; i++) ctx->Yield(true);    // let the watcher arm its poll
        }                                                   // parked watcher outlives the Reactor

        ASSERT_EQ(write(fds[1], "x", 1), 1);                // wake it after its Reactor is gone
        for (int i = 0; i < 6; i++) ctx->Yield(true);

        EXPECT_EQ(st.readyCount, 0);                        // a dead Reactor never calls back

        close(fds[0]);
        close(fds[1]);
    });
}

TEST(ReactorTest, DestroyedWithIdleWatcherDoesNotBlockShutdown)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    // Keep both sockets open through shutdown. A watchdog supplies readiness only if shutdown
    // stalls, so a regression fails an assertion instead of hanging the entire test process.
    //
    std::binary_semaphore shutdownFinished{0};
    bool forcedWake = false;
    std::thread watchdog([&]
    {
        if (!shutdownFinished.try_acquire_for(std::chrono::seconds(2)))
        {
            forcedWake = true;
            write(fds[1], "x", 1);
        }
    });

    test::RunInCooperator([&](coop::Context* ctx)
    {
        State st;
        coop::io::Reactor reactor(ctx->GetCooperator(), &OnReady, &OnTimeout, &st);
        reactor.Watch(fds[0], POLLIN);
        coop::time::Sleep(ctx, std::chrono::milliseconds(5));
    });

    shutdownFinished.release();
    watchdog.join();
    EXPECT_FALSE(forcedWake) << "Idle reactor poll prevented cooperator shutdown";
    close(fds[0]);
    close(fds[1]);
}
