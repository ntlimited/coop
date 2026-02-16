#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/coordinator.h"
#include "coop/self.h"

#include "coop/io/connect.h"
#include "coop/io/descriptor.h"
#include "coop/io/handle.h"
#include "coop/io/recv.h"
#include "coop/io/resolve.h"
#include "coop/io/send.h"
#include "coop/io/sendfile.h"
#include "coop/io/splice.h"

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
