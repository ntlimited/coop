#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/coordinator.h"
#include "coop/self.h"

#include "coop/io/descriptor.h"
#include "coop/io/handle.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"

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
            reader, buf, sizeof(buf),
            coop::time::Interval(50));

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
            reader, buf, sizeof(buf),
            coop::time::Interval(5000));

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

        int result = handle;
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
