#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "coop/continuation.h"
#include "coop/coordinator.h"
#include "coop/coordinate_with.h"
#include "coop/self.h"
#include "coop/signal.h"

#include "coop/io/accept.h"
#include "coop/io/armed_accept.h"
#include "coop/io/connect.h"
#include "coop/io/descriptor.h"
#include "coop/io/handle.h"
#include "coop/io/poll.h"
#include "coop/io/recv.h"
#include "coop/io/resolve.h"
#include "coop/io/send.h"
#include "coop/io/block_limits.h"
#include "coop/io/pipe_pool.h"
#include "coop/io/sendfile.h"
#include "coop/io/splice.h"
#include "coop/io/statx.h"
#include "coop/io/shutdown_on_kill.h"
#include "coop/io/uring.h"

#include "coop/time/interval.h"

#include "test_helpers.h"

namespace
{

// Helper that creates a AF_UNIX socketpair and wraps both ends in Descriptors.
//
struct SocketPair
{
    int fds[2];

    SocketPair()
    {
        int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(ret == 0);
        std::ignore = ret;
    }

    ~SocketPair()
    {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
};

struct ListeningSocket
{
    int fd{-1};

    ListeningSocket()
    {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        assert(fd >= 0);

        int on = 1;
        int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        assert(ret == 0);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        ret = bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        assert(ret == 0);
        ret = listen(fd, 8);
        assert(ret == 0);
    }

    ~ListeningSocket()
    {
        if (fd >= 0) close(fd);
    }
};

} // end anonymous namespace

TEST(IoTest, RecvCompletes)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor reader(sp.fds[0], uring);
        coop::io::Descriptor writer(sp.fds[1], uring);

        const char* msg = "hello";
        int sent = coop::io::Send(writer, msg, strlen(msg));
        ASSERT_EQ(sent, 5);

        char buf[64] = {};
        int received = coop::io::Recv(reader, buf, sizeof(buf));
        ASSERT_EQ(received, 5);
        EXPECT_EQ(memcmp(buf, "hello", 5), 0);
    });
}

TEST(IoTest, RecvTimesOut)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor reader(sp.fds[0], uring);

        // Recv on an empty socket with a short timeout
        //
        char buf[64] = {};
        int result = coop::io::Recv(
            reader, buf, sizeof(buf), 0,
            std::chrono::milliseconds(50));

        EXPECT_EQ(result, -ETIMEDOUT);
    });
}

TEST(IoTest, RecvCompletesBeforeTimeout)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor reader(sp.fds[0], uring);
        coop::io::Descriptor writer(sp.fds[1], uring);

        // Write first, then recv with a generous timeout — data should arrive immediately
        //
        const char* msg = "world";
        int sent = coop::io::Send(writer, msg, strlen(msg));
        ASSERT_EQ(sent, 5);

        char buf[64] = {};
        int result = coop::io::Recv(
            reader, buf, sizeof(buf), 0,
            std::chrono::milliseconds(5000));

        ASSERT_EQ(result, 5);
        EXPECT_EQ(memcmp(buf, "world", 5), 0);
    });
}

TEST(IoTest, CancelPendingRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor reader(sp.fds[0], uring);

        coop::Coordinator coord;
        coop::io::Handle handle(ctx, reader, &coord);

        char buf[64] = {};
        bool ok = coop::io::Recv(handle, buf, sizeof(buf));
        ASSERT_TRUE(ok);

        // Cancel the in-flight recv
        //
        handle.Cancel();

        int result = handle.Wait();
        EXPECT_EQ(result, -ECANCELED);
    });
}

// A continuation registered on a Handle's coordinator fires straight from the io_uring CQE: the
// completion runs Finalize -> coord.Release(ctx, false), which drains the continuation as a
// function call (no extra context). This is the async IO decomposition the continuation work is
// for -- register the next stage on the completion instead of parking a context to await it.
//
TEST(IoTest, ContinuationFiresFromCqe)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor reader(sp.fds[0], uring);

        coop::Coordinator coord;
        coop::io::Handle handle(ctx, reader, &coord);

        char buf[64] = {};
        ASSERT_TRUE(coop::io::Recv(handle, buf, sizeof(buf)));

        bool fired = false;
        auto c = coord.Continue([&](coop::Coordinator*)
        {
            fired = true;
            return handle.Result();
        });

        // Drive the recv to completion from the peer end.
        //
        const char msg[] = "hello";
        ASSERT_EQ(::write(sp.fds[1], msg, sizeof(msg)), (ssize_t)sizeof(msg));

        // Await parks this context; the loop polls uring, the CQE runs Finalize -> Release ->
        // the continuation drains and fires -> the completion latch wakes us with the result.
        //
        int n = c.Await();
        EXPECT_TRUE(fired);
        EXPECT_EQ(n, (int)sizeof(msg));
        EXPECT_STREQ(buf, "hello");
    });
}

// Same path with a detached (self-owning) continuation: it fires from the CQE drain, self-frees,
// and signals completion. No awaiter is parked on the IO at all -- the high-fan-out shape.
//
TEST(IoTest, DetachedContinuationFiresFromCqe)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor reader(sp.fds[0], uring);

        coop::Coordinator coord;
        coop::io::Handle handle(ctx, reader, &coord);

        char buf[64] = {};
        ASSERT_TRUE(coop::io::Recv(handle, buf, sizeof(buf)));

        coop::Signal done(ctx);
        int received = -1;
        coord.ContinueDetached([&](coop::Coordinator*)
        {
            received = handle.Result();
            done.Notify(nullptr, /*schedule=*/false);   // wake the test from the drain
        });

        const char msg[] = "world";
        ASSERT_EQ(::write(sp.fds[1], msg, sizeof(msg)), (ssize_t)sizeof(msg));

        done.Wait(ctx);                                 // until the detached continuation fires
        EXPECT_EQ(received, (int)sizeof(msg));
        EXPECT_STREQ(buf, "world");
    });
}

TEST(IoTest, RAIICancelOnDestroy)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor reader(sp.fds[0], uring);

        // Submit a recv inside an inner scope; the Handle destructor should cancel and drain
        //
        {
            coop::Coordinator coord;
            coop::io::Handle handle(ctx, reader, &coord);

            char buf[64] = {};
            bool ok = coop::io::Recv(handle, buf, sizeof(buf));
            ASSERT_TRUE(ok);

            // Handle goes out of scope here — destructor calls Cancel() + Flash()
            //
        }

        // If we get here without crashing, the RAII cancel worked
        //
        SUCCEED();
    });
}

TEST(IoTest, AsyncAcceptCanBeKilledWithoutInboundConnection)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ListeningSocket listener;
        auto* uring = coop::GetUring();

        coop::Coordinator ready;
        ready.TryAcquire(ctx);

        coop::Context::Handle childHandle;
        bool killed = false;
        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::io::Descriptor desc(coop::io::borrowed, listener.fd, uring);
            coop::Coordinator coord;
            coop::io::Handle handle(child, desc, &coord);

            bool ok = coop::io::Accept(handle);
            ASSERT_TRUE(ok);

            ready.Release(child, false);

            auto result = coop::CoordinateWithKill(child, &coord);
            killed = result.Killed();
            if (!result.Killed())
            {
                coord.Release(child, false);
                int acceptedFd = handle.Result();
                if (acceptedFd >= 0)
                {
                    ::close(acceptedFd);
                }
            }
            done = true;
        }, &childHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        childHandle.Kill();
        while (!done)
        {
            ctx->Yield(true);
        }

        EXPECT_TRUE(killed);
    });
}

TEST(IoTest, WaitKillCancelsBlockedRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor reader(sp.fds[0], uring);

        coop::Coordinator ready;
        ready.TryAcquire(ctx);

        coop::Context::Handle childHandle;
        int result = 0;
        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::Coordinator coord;
            coop::io::Handle handle(child, reader, &coord);

            char buf[64] = {};
            bool ok = coop::io::Recv(handle, buf, sizeof(buf));
            ASSERT_TRUE(ok);

            ready.Release(child, false);

            result = handle.WaitKill();
            done = true;
        }, &childHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        childHandle.Kill();
        while (!done)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(result, -ECANCELED);
    });
}

TEST(IoTest, RecvKillReturnsCanceledWithoutInboundData)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::Coordinator ready;
        ready.TryAcquire(ctx);

        coop::Context::Handle childHandle;
        int result = 0;
        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::io::Descriptor reader(sp.fds[0], uring);
            char buf[64] = {};
            ready.Release(child, false);
            result = coop::io::RecvKill(reader, buf, sizeof(buf));
            done = true;
        }, &childHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        childHandle.Kill();
        while (!done)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(result, -ECANCELED);
    });
}

TEST(IoTest, PollKillReturnsCanceledWithoutReadiness)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::Coordinator ready;
        ready.TryAcquire(ctx);

        coop::Context::Handle childHandle;
        int result = 0;
        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::io::Descriptor reader(sp.fds[0], uring);
            ready.Release(child, false);
            result = coop::io::PollKill(reader, POLLIN);
            done = true;
        }, &childHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        childHandle.Kill();
        while (!done)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(result, -ECANCELED);
    });
}

TEST(IoTest, ShutdownOnKillGuardWakesBlockingRecv)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::Coordinator ready;
        ready.TryAcquire(ctx);

        coop::Context::Handle childHandle;
        int result = 1;
        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::io::Descriptor reader(sp.fds[0], uring);
            coop::io::ShutdownOnKillGuard shutdownGuard(child, reader);

            char buf[64] = {};
            ready.Release(child, false);
            result = coop::io::Recv(reader, buf, sizeof(buf));
            done = true;
        }, &childHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        childHandle.Kill();
        while (!done)
        {
            ctx->Yield(true);
        }

        EXPECT_LE(result, 0);
    });
}

// -------------------------------------------------------------------------------------
// Sendfile tests
// -------------------------------------------------------------------------------------

TEST(IoTest, Sendfile)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();

        // Set sender non-blocking (required for sendfile + io::Poll)
        //
        fcntl(sp.fds[1], F_SETFL, fcntl(sp.fds[1], F_GETFL) | O_NONBLOCK);

        coop::io::Descriptor reader(sp.fds[0], uring);
        coop::io::Descriptor writer(sp.fds[1], uring);

        // Create a temp file with known content
        //
        char tmpPath[] = "/tmp/coop_sendfile_XXXXXX";
        int fileFd = mkstemp(tmpPath);
        ASSERT_GE(fileFd, 0);
        unlink(tmpPath);

        const char* fileData = "sendfile test data — hello from the kernel!";
        size_t fileLen = strlen(fileData);
        [[maybe_unused]] ssize_t w = ::write(fileFd, fileData, fileLen);
        assert(w == (ssize_t)fileLen);

        // Sendfile the entire file
        //
        int sent = coop::io::SendfileAll(writer, fileFd, 0, fileLen);
        ASSERT_EQ(sent, (int)fileLen);

        ::close(fileFd);

        // Read it back and verify
        //
        char buf[128] = {};
        int received = coop::io::Recv(reader, buf, sizeof(buf));
        ASSERT_EQ(received, (int)fileLen);
        EXPECT_EQ(memcmp(buf, fileData, fileLen), 0);
    });
}

TEST(IoTest, SendfilePartialOffset)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();

        fcntl(sp.fds[1], F_SETFL, fcntl(sp.fds[1], F_GETFL) | O_NONBLOCK);

        coop::io::Descriptor reader(sp.fds[0], uring);
        coop::io::Descriptor writer(sp.fds[1], uring);

        // Create a file with known content
        //
        char tmpPath[] = "/tmp/coop_sendfile_XXXXXX";
        int fileFd = mkstemp(tmpPath);
        ASSERT_GE(fileFd, 0);
        unlink(tmpPath);

        const char* fileData = "ABCDEFGHIJ";
        [[maybe_unused]] ssize_t w = ::write(fileFd, fileData, 10);
        assert(w == 10);

        // Send only bytes 3-7 ("DEFGH")
        //
        int sent = coop::io::SendfileAll(writer, fileFd, 3, 5);
        ASSERT_EQ(sent, 5);

        ::close(fileFd);

        char buf[64] = {};
        int received = coop::io::Recv(reader, buf, sizeof(buf));
        ASSERT_EQ(received, 5);
        EXPECT_EQ(memcmp(buf, "DEFGH", 5), 0);
    });
}

TEST(IoTest, SendfileAllKillReturnsCanceledWhenOutputNotWritable)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();

        fcntl(sp.fds[1], F_SETFL, fcntl(sp.fds[1], F_GETFL) | O_NONBLOCK);

        int sndbuf = 4096;
        int ret = setsockopt(sp.fds[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        ASSERT_EQ(ret, 0);

        char fill[1024];
        memset(fill, 'x', sizeof(fill));
        for (;;)
        {
            ssize_t n = ::send(sp.fds[1], fill, sizeof(fill), MSG_DONTWAIT);
            if (n > 0)
            {
                continue;
            }

            ASSERT_EQ(n, -1);
            ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }

        char tmpPath[] = "/tmp/coop_sendfile_kill_XXXXXX";
        int fileFd = mkstemp(tmpPath);
        ASSERT_GE(fileFd, 0);
        unlink(tmpPath);

        static constexpr size_t kFileSize = 1 << 20;
        char fileChunk[4096];
        memset(fileChunk, 's', sizeof(fileChunk));
        for (size_t written = 0; written < kFileSize; written += sizeof(fileChunk))
        {
            [[maybe_unused]] ssize_t w = ::write(fileFd, fileChunk, sizeof(fileChunk));
            assert(w == (ssize_t)sizeof(fileChunk));
        }

        coop::Coordinator ready;
        ready.TryAcquire(ctx);

        coop::Context::Handle childHandle;
        int result = 0;
        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::io::Descriptor writer(sp.fds[1], uring);
            ready.Release(child, false);
            result = coop::io::SendfileAllKill(writer, fileFd, 0, kFileSize);
            done = true;
        }, &childHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        childHandle.Kill();
        while (!done)
        {
            ctx->Yield(true);
        }

        ::close(fileFd);
        EXPECT_EQ(result, -ECANCELED);
    });
}

// -------------------------------------------------------------------------------------
// Splice tests
// -------------------------------------------------------------------------------------

TEST(IoTest, Splice)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Two socketpairs: inject data into sp[1], splice sp[0]→sp2[1], read from sp2[0]
        //
        SocketPair sp, sp2;
        auto* uring = coop::GetUring();

        fcntl(sp.fds[0], F_SETFL, fcntl(sp.fds[0], F_GETFL) | O_NONBLOCK);
        fcntl(sp2.fds[1], F_SETFL, fcntl(sp2.fds[1], F_GETFL) | O_NONBLOCK);

        coop::io::Descriptor in(sp.fds[0], uring);
        coop::io::Descriptor out(sp2.fds[1], uring);

        // Write test data into the input socket
        //
        const char* msg = "hello splice!";
        size_t msgLen = strlen(msg);
        [[maybe_unused]] ssize_t w = ::write(sp.fds[1], msg, msgLen);
        assert(w == (ssize_t)msgLen);

        // Create pipe for splice
        //
        int pipefd[2];
        ASSERT_EQ(pipe2(pipefd, O_NONBLOCK), 0);

        int transferred = coop::io::Splice(in, out, pipefd, 65536);
        ASSERT_EQ(transferred, (int)msgLen);

        close(pipefd[0]);
        close(pipefd[1]);

        // Read from output and verify
        //
        char buf[64] = {};
        ssize_t r = ::read(sp2.fds[0], buf, sizeof(buf));
        ASSERT_EQ(r, (ssize_t)msgLen);
        EXPECT_EQ(memcmp(buf, msg, msgLen), 0);
    });
}

TEST(IoTest, SpliceEOF)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp, sp2;
        auto* uring = coop::GetUring();

        fcntl(sp.fds[0], F_SETFL, fcntl(sp.fds[0], F_GETFL) | O_NONBLOCK);
        fcntl(sp2.fds[1], F_SETFL, fcntl(sp2.fds[1], F_GETFL) | O_NONBLOCK);

        coop::io::Descriptor in(sp.fds[0], uring);
        coop::io::Descriptor out(sp2.fds[1], uring);

        // Write data then close the write end to produce EOF
        //
        [[maybe_unused]] ssize_t w = ::write(sp.fds[1], "data", 4);
        assert(w == 4);
        close(sp.fds[1]);
        sp.fds[1] = -1;

        int pipefd[2];
        ASSERT_EQ(pipe2(pipefd, O_NONBLOCK), 0);

        // First splice should transfer the data
        //
        int transferred = coop::io::Splice(in, out, pipefd, 65536);
        ASSERT_EQ(transferred, 4);

        // Second splice should return 0 (EOF)
        //
        int eof = coop::io::Splice(in, out, pipefd, 65536);
        EXPECT_EQ(eof, 0);

        close(pipefd[0]);
        close(pipefd[1]);
    });
}

TEST(IoTest, SpliceKillReturnsCanceledWhileWaitingForInput)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp, sp2;
        auto* uring = coop::GetUring();
        coop::Coordinator ready;
        ready.TryAcquire(ctx);

        fcntl(sp.fds[0], F_SETFL, fcntl(sp.fds[0], F_GETFL) | O_NONBLOCK);
        fcntl(sp2.fds[1], F_SETFL, fcntl(sp2.fds[1], F_GETFL) | O_NONBLOCK);

        coop::Context::Handle childHandle;
        int result = 0;
        bool done = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            coop::io::Descriptor in(sp.fds[0], uring);
            coop::io::Descriptor out(sp2.fds[1], uring);
            int pipefd[2];
            ASSERT_EQ(pipe2(pipefd, O_NONBLOCK), 0);

            ready.Release(child, false);
            result = coop::io::SpliceKill(in, out, pipefd, 65536);

            close(pipefd[0]);
            close(pipefd[1]);
            done = true;
        }, &childHandle);

        ready.Acquire(ctx);
        ready.Release(ctx, false);

        childHandle.Kill();
        while (!done)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(result, -ECANCELED);
    });
}

// -------------------------------------------------------------------------------------
// Resolve tests
// -------------------------------------------------------------------------------------

TEST(ResolveTest, Localhost)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        struct in_addr result;
        int ret = coop::io::Resolve4("localhost", &result);
        ASSERT_EQ(ret, 0);
        EXPECT_EQ(result.s_addr, htonl(INADDR_LOOPBACK));
    });
}

TEST(ResolveTest, NumericPassthrough)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        struct in_addr result;
        int ret = coop::io::Resolve4("127.0.0.1", &result);
        ASSERT_EQ(ret, 0);
        EXPECT_EQ(result.s_addr, htonl(INADDR_LOOPBACK));
    });
}

TEST(ResolveTest, PublicHostname)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        struct in_addr result;
        int ret = coop::io::Resolve4("dns.google", &result,
            std::chrono::seconds(10));
        ASSERT_EQ(ret, 0);

        // dns.google resolves to 8.8.8.8 or 8.8.4.4
        //
        struct in_addr expected1, expected2;
        inet_pton(AF_INET, "8.8.8.8", &expected1);
        inet_pton(AF_INET, "8.8.4.4", &expected2);
        EXPECT_TRUE(result.s_addr == expected1.s_addr || result.s_addr == expected2.s_addr);
    });
}

TEST(ResolveTest, NonExistent)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        struct in_addr result;
        int ret = coop::io::Resolve4("this.does.not.exist.example.", &result,
            std::chrono::seconds(10));
        EXPECT_EQ(ret, -ENOENT);
    });
}

TEST(ResolveTest, ConnectWithHostname)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        ASSERT_GE(fd, 0);

        auto* ring = coop::GetUring();
        coop::io::Descriptor desc(fd, ring);

        // Connect to dns.google on port 443 — just verify the connect succeeds
        //
        int ret = coop::io::Connect(desc, "dns.google", 443);
        EXPECT_GE(ret, 0);

        desc.Close();
    });
}

// -------------------------------------------------------------------------------------
// Statx
// -------------------------------------------------------------------------------------

TEST(IoTest, StatxFd)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        char tmpPath[] = "/tmp/coop_statx_XXXXXX";
        int fileFd = mkstemp(tmpPath);
        ASSERT_GE(fileFd, 0);
        unlink(tmpPath);

        const char* data = "0123456789";
        [[maybe_unused]] ssize_t w = ::write(fileFd, data, 10);
        assert(w == 10);

        struct statx stx = {};
        int ret = coop::io::StatxFd(fileFd, STATX_SIZE | STATX_TYPE, &stx);
        ASSERT_EQ(ret, 0);
        EXPECT_EQ(stx.stx_size, 10u);
        EXPECT_TRUE(S_ISREG(stx.stx_mode));

        ::close(fileFd);
    });
}

TEST(IoTest, StatxPath)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        char tmpPath[] = "/tmp/coop_statx_path_XXXXXX";
        int fileFd = mkstemp(tmpPath);
        ASSERT_GE(fileFd, 0);

        [[maybe_unused]] ssize_t w = ::write(fileFd, "abc", 3);
        assert(w == 3);

        struct statx stx = {};
        int ret = coop::io::Statx(tmpPath, 0, STATX_SIZE, &stx);
        ASSERT_EQ(ret, 0);
        EXPECT_EQ(stx.stx_size, 3u);

        // Missing path surfaces the errno through the checked return
        //
        ret = coop::io::Statx("/tmp/coop_statx_definitely_missing", 0, STATX_SIZE, &stx);
        EXPECT_EQ(ret, -ENOENT);

        ::close(fileFd);
        unlink(tmpPath);
    });
}

// -------------------------------------------------------------------------------------
// SpliceToFile
// -------------------------------------------------------------------------------------

TEST(IoTest, SpliceToFile)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();

        fcntl(sp.fds[0], F_SETFL, fcntl(sp.fds[0], F_GETFL) | O_NONBLOCK);
        coop::io::Descriptor in(sp.fds[0], uring);

        char tmpPath[] = "/tmp/coop_splice_file_XXXXXX";
        int fileFd = mkstemp(tmpPath);
        ASSERT_GE(fileFd, 0);
        unlink(tmpPath);

        const char* msg = "spliced to page cache";
        size_t msgLen = strlen(msg);
        [[maybe_unused]] ssize_t w = ::write(sp.fds[1], msg, msgLen);
        assert(w == (ssize_t)msgLen);

        // Pool-leased pipe, data lands at offset 4
        //
        coop::io::PipeLease pipe(uring->GetPipePool());
        ASSERT_TRUE(static_cast<bool>(pipe));

        int transferred = coop::io::SpliceToFile(in, fileFd, 4, pipe.Fds(), 65536);
        ASSERT_EQ(transferred, (int)msgLen);

        char buf[64] = {};
        ssize_t r = ::pread(fileFd, buf, sizeof(buf), 4);
        ASSERT_EQ(r, (ssize_t)msgLen);
        EXPECT_EQ(memcmp(buf, msg, msgLen), 0);

        ::close(fileFd);
    });
}

TEST(IoTest, PipePoolReuse)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto& pool = coop::GetUring()->GetPipePool();

        int first[2];
        {
            coop::io::PipeLease lease(pool);
            ASSERT_TRUE(static_cast<bool>(lease));
            first[0] = lease.Fds()[0];
            first[1] = lease.Fds()[1];
        }

        // A clean release caches the pipe; the next lease gets the same fds back
        //
        coop::io::PipeLease again(pool);
        ASSERT_TRUE(static_cast<bool>(again));
        EXPECT_EQ(again.Fds()[0], first[0]);
        EXPECT_EQ(again.Fds()[1], first[1]);
    });
}

// -------------------------------------------------------------------------------------
// Cooperative fairness: bulk sendfile must not monopolize the cooperator
// -------------------------------------------------------------------------------------

TEST(IoTest, SendfileAllYieldsUnderFastReader)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();

        fcntl(sp.fds[1], F_SETFL, fcntl(sp.fds[1], F_GETFL) | O_NONBLOCK);
        coop::io::Descriptor writer(sp.fds[1], uring);

        constexpr size_t kSize = 64 * 1024 * 1024;

        char tmpPath[] = "/tmp/coop_sendfile_yield_XXXXXX";
        int fileFd = mkstemp(tmpPath);
        ASSERT_GE(fileFd, 0);
        unlink(tmpPath);

        std::vector<char> block(1 << 20, 'x');
        for (int i = 0; i < 64; i++)
        {
            ASSERT_EQ(::write(fileFd, block.data(), block.size()),
                      (ssize_t)block.size());
        }

        // External drainer keeps the socket writable so sendfile rarely (or never)
        // blocks through Poll — without the yield budget, the sendfile context would
        // hold the cooperator for the whole transfer.
        //
        std::thread drainer([&]
        {
            std::vector<char> buf(1 << 20);
            size_t got = 0;
            while (got < kSize)
            {
                ssize_t r = ::recv(sp.fds[0], buf.data(), buf.size(), 0);
                if (r <= 0) break;
                got += static_cast<size_t>(r);
            }
        });

        bool done = false;
        int canaryRuns = 0;
        ctx->GetCooperator()->Spawn([&](coop::Context* c)
        {
            while (!done)
            {
                canaryRuns++;
                c->Yield(true);
            }
        });

        int sent = coop::io::SendfileAll(writer, fileFd, 0, kSize);
        done = true;
        drainer.join();

        EXPECT_EQ(sent, (int)kSize);

        // 64MB at a 2MB yield budget guarantees ~31 scheduler passes even when the
        // socket never pushes back; a generous floor keeps the assertion robust.
        //
        EXPECT_GE(canaryRuns, 8);

        ::close(fileFd);
    });
}

// -------------------------------------------------------------------------------------
// Block limits + io-wq observability
// -------------------------------------------------------------------------------------

TEST(IoTest, ProbeBlockLimits)
{
    // /tmp is backed by some device or tmpfs; either real limits or a clean
    // all-unknown result — never a crash or partial garbage.
    //
    auto limits = coop::io::ProbeBlockLimits("/tmp");
    if (limits.maxHwSectorsKb >= 0)
    {
        EXPECT_GT(limits.maxHwSectorsKb, 0);
    }

    auto missing = coop::io::ProbeBlockLimits("/definitely/not/a/path");
    EXPECT_EQ(missing.maxHwSectorsKb, -1);
    EXPECT_EQ(missing.nrRequests, -1);
    EXPECT_EQ(missing.maxSegments, -1);
}

TEST(IoTest, IowqWorkerCount)
{
    // Healthy steady state for a socket-driven test process: no io-wq workers.
    // The assertion allows some (buffered file IO from other tests may punt) but
    // the call itself must succeed.
    //
    int count = coop::io::IowqWorkerCount();
    EXPECT_GE(count, 0);
}

// -------------------------------------------------------------------------------------
// ArmedAccept: multishot accept with bounded-queue backpressure
// -------------------------------------------------------------------------------------

namespace
{

// Nonblocking connect: the test drives client and server from one cooperator thread, so
// a blocking connect against a full backlog would deadlock the whole test (nothing left
// to run the accept side). EINPROGRESS is success here — the kernel completes the
// handshake as the acceptor drains.
//
int ConnectLoopback(int listenFd)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
    {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 &&
        errno != EINPROGRESS)
    {
        close(fd);
        return -1;
    }
    return fd;
}

} // end anonymous namespace

TEST(IoTest, ArmedAcceptStreams)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ListeningSocket ls;
        auto* uring = coop::GetUring();
        coop::io::Descriptor listener(coop::io::borrowed, ls.fd, uring);

        coop::Coordinator coord;
        coop::io::ArmedAccept armed(ctx, listener, &coord, 16);
        armed.Arm();

        std::vector<int> clients;
        for (int i = 0; i < 3; i++)
        {
            int fd = ConnectLoopback(ls.fd);
            ASSERT_GE(fd, 0);
            clients.push_back(fd);
        }

        for (int i = 0; i < 3; i++)
        {
            int fd = armed.Next();
            ASSERT_GE(fd, 0);

            // Accepted with SOCK_NONBLOCK — required by the data-path engines
            //
            EXPECT_NE(fcntl(fd, F_GETFL) & O_NONBLOCK, 0);
            close(fd);
        }
        EXPECT_EQ(armed.Delivered(), 3u);

        for (int fd : clients)
        {
            close(fd);
        }
    });
}

TEST(IoTest, ArmedAcceptBackpressurePausesAndResumes)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ListeningSocket ls;
        auto* uring = coop::GetUring();
        coop::io::Descriptor listener(coop::io::borrowed, ls.fd, uring);

        coop::Coordinator coord;
        coop::io::ArmedAccept armed(ctx, listener, &coord, 4);
        armed.Arm();

        // Flood past the pending bound; the kernel-side backlog absorbs the rest while
        // the multishot is paused.
        //
        std::vector<int> clients;
        for (int i = 0; i < 12; i++)
        {
            int fd = ConnectLoopback(ls.fd);
            ASSERT_GE(fd, 0);
            clients.push_back(fd);
        }

        // Let the CQE stream reach the bound (Yield drives the scheduler's Poll)
        //
        for (int i = 0; i < 50; i++)
        {
            ctx->Yield(true);
        }

        // Every connection is eventually served across pause/resume cycles
        //
        for (int i = 0; i < 12; i++)
        {
            int fd = armed.Next();
            ASSERT_GE(fd, 0) << "connection " << i;
            close(fd);
        }
        EXPECT_EQ(armed.Delivered(), 12u);
        EXPECT_GE(armed.Paused(), 1u);

        for (int fd : clients)
        {
            close(fd);
        }
    });
}

TEST(IoTest, ArmedAcceptTeardownClosesQueuedFds)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ListeningSocket ls;
        auto* uring = coop::GetUring();
        coop::io::Descriptor listener(coop::io::borrowed, ls.fd, uring);

        std::vector<int> clients;
        {
            coop::Coordinator coord;
            coop::io::ArmedAccept armed(ctx, listener, &coord, 8);
            armed.Arm();

            for (int i = 0; i < 3; i++)
            {
                int fd = ConnectLoopback(ls.fd);
                ASSERT_GE(fd, 0);
                clients.push_back(fd);
            }

            // Consume one, leave the rest queued; the destructor must close them
            //
            int fd = armed.Next();
            ASSERT_GE(fd, 0);
            close(fd);
        }

        for (int fd : clients)
        {
            close(fd);
        }
    });
}
