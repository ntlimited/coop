#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "coop/self.h"
#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/io/uring.h"

#include "test_helpers.h"

namespace
{

bool FdOpen(int fd)
{
    return ::fcntl(fd, F_GETFD) != -1;
}

struct RawPair
{
    int fds[2];

    RawPair()
    {
        int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(ret == 0);
        std::ignore = ret;
    }

    ~RawPair()
    {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
};

} // end anonymous namespace

// A registered descriptor still moves bytes — Handle::Submit rewrites the SQE onto the
// fixed-file index, so this exercises the IOSQE_FIXED_FILE path end to end.
//
TEST(RegisteredTest, RegisteredDescriptorDoesIo)
{
    test::RunInCooperator([](coop::Context*)
    {
        RawPair sp;
        auto* uring = coop::GetUring();

        coop::io::Descriptor reg(coop::io::registered, sp.fds[0], uring);
        sp.fds[0] = -1;
        coop::io::Descriptor peer(coop::io::borrowed, sp.fds[1], uring);

        ASSERT_GE(reg.m_registeredIndex, 0);

        ASSERT_EQ(coop::io::SendAll(reg, "ping", 4), 4);
        char buf[8] = {};
        ASSERT_EQ(coop::io::Recv(peer, buf, sizeof(buf)), 4);
        EXPECT_EQ(memcmp(buf, "ping", 4), 0);
    });
}

// Explicit Close() on a registered descriptor must actually close the fd. The failure
// mode this guards: IORING_OP_CLOSE rejects IOSQE_FIXED_FILE with -EBADF, so a close
// submitted through the fixed-file rewrite never reaches the fd — Close() "succeeds"
// (m_fd = -1) while the fd leaks for the process lifetime and the slot keeps a live IO
// path to the socket.
//
TEST(RegisteredTest, ExplicitCloseReallyCloses)
{
    test::RunInCooperator([](coop::Context*)
    {
        RawPair sp;
        auto* uring = coop::GetUring();

        int rawFd = sp.fds[0];
        coop::io::Descriptor reg(coop::io::registered, rawFd, uring);
        sp.fds[0] = -1;

        ASSERT_GE(reg.m_registeredIndex, 0);

        EXPECT_EQ(reg.Close(), 0);
        EXPECT_LT(reg.m_registeredIndex, 0);
        EXPECT_FALSE(FdOpen(rawFd));

        // The peer sees EOF — the socket is genuinely closed, not pinned by the slot
        //
        coop::io::Descriptor peer(coop::io::borrowed, sp.fds[1], uring);
        char buf[4];
        EXPECT_LE(coop::io::Recv(peer, buf, sizeof(buf), 0,
                                 std::chrono::milliseconds(200)), 0);
    });
}

// Release() hands the raw fd back to the caller; the ring's fixed-file slot must not
// keep pinning the file past that handoff.
//
TEST(RegisteredTest, ReleaseUnregisters)
{
    test::RunInCooperator([](coop::Context*)
    {
        RawPair sp;
        auto* uring = coop::GetUring();

        coop::io::Descriptor reg(coop::io::registered, sp.fds[0], uring);
        sp.fds[0] = -1;
        ASSERT_GE(reg.m_registeredIndex, 0);

        int fd = reg.Release();
        EXPECT_GE(fd, 0);
        EXPECT_LT(reg.m_registeredIndex, 0);
        EXPECT_TRUE(FdOpen(fd));

        ::close(fd);
    });
}

// Table exhaustion degrades gracefully: the descriptor stays usable as a plain fd with
// m_registeredIndex == -1, and IO still flows.
//
TEST(RegisteredTest, ExhaustionDegradesToPlainFd)
{
    test::RunInCooperator([](coop::Context*)
    {
        auto* uring = coop::GetUring();

        // Fill the table (default 64 slots) with pipe fds
        //
        std::vector<std::unique_ptr<coop::io::Descriptor>> hogs;
        std::vector<int> pipes;
        for (int i = 0; i < 70; i++)
        {
            int pfd[2];
            ASSERT_EQ(::pipe(pfd), 0);
            pipes.push_back(pfd[1]);
            hogs.push_back(std::make_unique<coop::io::Descriptor>(
                coop::io::registered, pfd[0], uring));
            if (hogs.back()->m_registeredIndex < 0)
            {
                break;
            }
        }
        ASSERT_LT(hogs.back()->m_registeredIndex, 0) << "table never exhausted";

        // The unregistered descriptor still does IO through the plain-fd path
        //
        RawPair sp;
        coop::io::Descriptor plainAfterExhaustion(coop::io::registered, sp.fds[0], uring);
        sp.fds[0] = -1;
        EXPECT_LT(plainAfterExhaustion.m_registeredIndex, 0);

        coop::io::Descriptor peer(coop::io::borrowed, sp.fds[1], uring);
        ASSERT_EQ(coop::io::SendAll(plainAfterExhaustion, "ok", 2), 2);
        char buf[4] = {};
        ASSERT_EQ(coop::io::Recv(peer, buf, sizeof(buf)), 2);

        for (int fd : pipes)
        {
            ::close(fd);
        }
    });
}
