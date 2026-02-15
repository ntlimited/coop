#include <cassert>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <benchmark/benchmark.h>

static constexpr int MSG_SIZE = 64;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void MakeSocketPair(int fds[2])
{
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(ret == 0);
    (void)ret;
}

static void FullSend(int fd, const void* buf, int len)
{
    const char* p = static_cast<const char*>(buf);
    int remaining = len;
    while (remaining > 0)
    {
        ssize_t n = send(fd, p, remaining, 0);
        assert(n > 0);
        p += n;
        remaining -= n;
    }
}

static void FullRecv(int fd, void* buf, int len)
{
    char* p = static_cast<char*>(buf);
    int remaining = len;
    while (remaining > 0)
    {
        ssize_t n = recv(fd, p, remaining, 0);
        assert(n > 0);
        p += n;
        remaining -= n;
    }
}

// ---------------------------------------------------------------------------
// 1. Blocking (pthreads + blocking send/recv)
// ---------------------------------------------------------------------------
//
// Thread-per-connection model. Blocking send/recv on a socketpair naturally serializes the two
// sides — no mutex/condvar needed.
//

// Single thread. send + recv per iteration. Pure syscall cost, no thread switching.
//
static void BM_Blocking_RoundTrip(benchmark::State& state)
{
    int fds[2];
    MakeSocketPair(fds);

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};

    for (auto _ : state)
    {
        FullSend(fds[0], msg, MSG_SIZE);
        FullRecv(fds[1], buf, MSG_SIZE);
    }

    close(fds[0]);
    close(fds[1]);
}
BENCHMARK(BM_Blocking_RoundTrip);

// Two threads. Parent: send -> recv. Child: recv -> send. Blocking recv is the synchronization.
//
static void BM_Blocking_PingPong(benchmark::State& state)
{
    int fds[2];
    MakeSocketPair(fds);

    volatile bool done = false;

    std::thread child([&]
    {
        char buf[MSG_SIZE] = {};
        while (!done)
        {
            ssize_t n = recv(fds[1], buf, MSG_SIZE, 0);
            if (n <= 0 || done) break;
            FullSend(fds[1], buf, n);
        }
    });

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};

    for (auto _ : state)
    {
        FullSend(fds[0], msg, MSG_SIZE);
        FullRecv(fds[0], buf, MSG_SIZE);
    }

    // Unblock child's recv and join
    //
    done = true;
    FullSend(fds[0], msg, MSG_SIZE);
    child.join();

    close(fds[0]);
    close(fds[1]);
}
BENCHMARK(BM_Blocking_PingPong);

// N child threads, each on its own socketpair. Parent drives round-robin.
//
static void BM_Blocking_PingPong_Scale(benchmark::State& state)
{
    const int N = state.range(0);

    std::vector<int> writerFds(N);
    std::vector<int> readerFds(N);
    for (int i = 0; i < N; i++)
    {
        int fds[2];
        MakeSocketPair(fds);
        writerFds[i] = fds[0];
        readerFds[i] = fds[1];
    }

    volatile bool done = false;
    std::vector<std::thread> children;

    for (int i = 0; i < N; i++)
    {
        children.emplace_back([&done, fd = readerFds[i]]
        {
            char buf[MSG_SIZE] = {};
            while (!done)
            {
                ssize_t n = recv(fd, buf, MSG_SIZE, 0);
                if (n <= 0 || done) break;
                FullSend(fd, buf, n);
            }
        });
    }

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};

    for (auto _ : state)
    {
        for (int i = 0; i < N; i++)
        {
            FullSend(writerFds[i], msg, MSG_SIZE);
            FullRecv(writerFds[i], buf, MSG_SIZE);
        }
    }

    // Drain all children
    //
    done = true;
    for (int i = 0; i < N; i++)
        FullSend(writerFds[i], msg, MSG_SIZE);
    for (auto& t : children)
        t.join();

    for (int i = 0; i < N; i++)
    {
        close(writerFds[i]);
        close(readerFds[i]);
    }
}
BENCHMARK(BM_Blocking_PingPong_Scale)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32);

// ---------------------------------------------------------------------------
// 2. Poll (single-threaded event loop)
// ---------------------------------------------------------------------------
//
// Analogous to coop's unregistered FDs — the fd set is passed to the kernel on every call.
// A readiness check (poll()) precedes each IO operation.
//

// poll(POLLOUT) -> send -> poll(POLLIN) -> recv. Two poll() calls per iteration.
//
static void BM_Poll_RoundTrip(benchmark::State& state)
{
    int fds[2];
    MakeSocketPair(fds);

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};

    for (auto _ : state)
    {
        struct pollfd pfd;

        pfd = {fds[0], POLLOUT, 0};
        poll(&pfd, 1, -1);
        FullSend(fds[0], msg, MSG_SIZE);

        pfd = {fds[1], POLLIN, 0};
        poll(&pfd, 1, -1);
        FullRecv(fds[1], buf, MSG_SIZE);
    }

    close(fds[0]);
    close(fds[1]);
}
BENCHMARK(BM_Poll_RoundTrip);

// Single-threaded 4-step sequence per iteration. Both ends are in the same thread, so the
// linear sequence is correct.
//
static void BM_Poll_PingPong(benchmark::State& state)
{
    int fds[2];
    MakeSocketPair(fds);

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};

    for (auto _ : state)
    {
        struct pollfd pfd;

        // Parent sends on fds[0]
        //
        pfd = {fds[0], POLLOUT, 0};
        poll(&pfd, 1, -1);
        FullSend(fds[0], msg, MSG_SIZE);

        // "Ponger" receives on fds[1]
        //
        pfd = {fds[1], POLLIN, 0};
        poll(&pfd, 1, -1);
        FullRecv(fds[1], buf, MSG_SIZE);

        // "Ponger" sends back on fds[1]
        //
        pfd = {fds[1], POLLOUT, 0};
        poll(&pfd, 1, -1);
        FullSend(fds[1], buf, MSG_SIZE);

        // Parent receives on fds[0]
        //
        pfd = {fds[0], POLLIN, 0};
        poll(&pfd, 1, -1);
        FullRecv(fds[0], buf, MSG_SIZE);
    }

    close(fds[0]);
    close(fds[1]);
}
BENCHMARK(BM_Poll_PingPong);

// N socketpairs, driven round-robin. Each pair gets the same 4-step treatment.
//
static void BM_Poll_PingPong_Scale(benchmark::State& state)
{
    const int N = state.range(0);

    std::vector<int> writerFds(N);
    std::vector<int> readerFds(N);
    for (int i = 0; i < N; i++)
    {
        int fds[2];
        MakeSocketPair(fds);
        writerFds[i] = fds[0];
        readerFds[i] = fds[1];
    }

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};

    for (auto _ : state)
    {
        struct pollfd pfd;

        for (int i = 0; i < N; i++)
        {
            pfd = {writerFds[i], POLLOUT, 0};
            poll(&pfd, 1, -1);
            FullSend(writerFds[i], msg, MSG_SIZE);

            pfd = {readerFds[i], POLLIN, 0};
            poll(&pfd, 1, -1);
            FullRecv(readerFds[i], buf, MSG_SIZE);

            pfd = {readerFds[i], POLLOUT, 0};
            poll(&pfd, 1, -1);
            FullSend(readerFds[i], buf, MSG_SIZE);

            pfd = {writerFds[i], POLLIN, 0};
            poll(&pfd, 1, -1);
            FullRecv(writerFds[i], buf, MSG_SIZE);
        }
    }

    for (int i = 0; i < N; i++)
    {
        close(writerFds[i]);
        close(readerFds[i]);
    }
}
BENCHMARK(BM_Poll_PingPong_Scale)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32);

// ---------------------------------------------------------------------------
// 3. Epoll (single-threaded event loop)
// ---------------------------------------------------------------------------
//
// Analogous to coop's registered FDs — fds are added to the epoll instance once at setup via
// epoll_ctl(ADD), then only epoll_wait() is called in the hot loop.
//

// fds[0] registered EPOLLOUT, fds[1] registered EPOLLIN. No MOD calls needed — each fd has a
// fixed role.
//
static void BM_Epoll_RoundTrip(benchmark::State& state)
{
    int fds[2];
    MakeSocketPair(fds);

    int epfd = epoll_create1(0);
    assert(epfd >= 0);

    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data.fd = fds[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev);

    ev.events = EPOLLIN;
    ev.data.fd = fds[1];
    epoll_ctl(epfd, EPOLL_CTL_ADD, fds[1], &ev);

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};
    struct epoll_event events[1];

    for (auto _ : state)
    {
        epoll_wait(epfd, events, 1, -1);
        FullSend(fds[0], msg, MSG_SIZE);

        epoll_wait(epfd, events, 1, -1);
        FullRecv(fds[1], buf, MSG_SIZE);
    }

    close(epfd);
    close(fds[0]);
    close(fds[1]);
}
BENCHMARK(BM_Epoll_RoundTrip);

// Both fds registered with EPOLLIN|EPOLLOUT at setup. Same 4-step sequence as poll, but
// epoll_wait replaces poll(). No MOD calls in the hot loop.
//
static void BM_Epoll_PingPong(benchmark::State& state)
{
    int fds[2];
    MakeSocketPair(fds);

    int epfd = epoll_create1(0);
    assert(epfd >= 0);

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = fds[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev);

    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = fds[1];
    epoll_ctl(epfd, EPOLL_CTL_ADD, fds[1], &ev);

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};
    struct epoll_event events[2];

    for (auto _ : state)
    {
        epoll_wait(epfd, events, 2, -1);
        FullSend(fds[0], msg, MSG_SIZE);

        epoll_wait(epfd, events, 2, -1);
        FullRecv(fds[1], buf, MSG_SIZE);

        epoll_wait(epfd, events, 2, -1);
        FullSend(fds[1], buf, MSG_SIZE);

        epoll_wait(epfd, events, 2, -1);
        FullRecv(fds[0], buf, MSG_SIZE);
    }

    close(epfd);
    close(fds[0]);
    close(fds[1]);
}
BENCHMARK(BM_Epoll_PingPong);

// All 2*N fds added to a single epoll instance at setup. Same round-robin 4-step-per-pair loop.
//
static void BM_Epoll_PingPong_Scale(benchmark::State& state)
{
    const int N = state.range(0);

    std::vector<int> writerFds(N);
    std::vector<int> readerFds(N);

    int epfd = epoll_create1(0);
    assert(epfd >= 0);

    for (int i = 0; i < N; i++)
    {
        int fds[2];
        MakeSocketPair(fds);
        writerFds[i] = fds[0];
        readerFds[i] = fds[1];

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = fds[0];
        epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev);

        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = fds[1];
        epoll_ctl(epfd, EPOLL_CTL_ADD, fds[1], &ev);
    }

    char msg[MSG_SIZE] = {};
    char buf[MSG_SIZE] = {};
    struct epoll_event events[64];

    for (auto _ : state)
    {
        for (int i = 0; i < N; i++)
        {
            epoll_wait(epfd, events, 64, -1);
            FullSend(writerFds[i], msg, MSG_SIZE);

            epoll_wait(epfd, events, 64, -1);
            FullRecv(readerFds[i], buf, MSG_SIZE);

            epoll_wait(epfd, events, 64, -1);
            FullSend(readerFds[i], buf, MSG_SIZE);

            epoll_wait(epfd, events, 64, -1);
            FullRecv(writerFds[i], buf, MSG_SIZE);
        }
    }

    close(epfd);
    for (int i = 0; i < N; i++)
    {
        close(writerFds[i]);
        close(readerFds[i]);
    }
}
BENCHMARK(BM_Epoll_PingPong_Scale)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32);
