#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "coop/self.h"
#include "coop/cooperator.h"
#include "coop/thread.h"
#include "coop/io/buffer_arena.h"
#include "coop/io/fixed_buffer.h"
#include "coop/io/read.h"
#include "coop/io/send_zc.h"
#include "coop/io/write.h"
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

// SEND_ZC does not support AF_UNIX — zc tests need a real TCP pair.
//
struct TcpPair
{
    int fds[2] = { -1, -1 };

    TcpPair()
    {
        int listenFd = socket(AF_INET, SOCK_STREAM, 0);
        assert(listenFd >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        int ret = bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        assert(ret == 0);
        ret = listen(listenFd, 1);
        assert(ret == 0);
        socklen_t len = sizeof(addr);
        getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
        fds[0] = socket(AF_INET, SOCK_STREAM, 0);
        ret = connect(fds[0], reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        assert(ret == 0);
        fds[1] = accept(listenFd, nullptr, nullptr);
        assert(fds[1] >= 0);
        close(listenFd);
        std::ignore = ret;
    }

    ~TcpPair()
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

// -------------------------------------------------------------------------------------
// BufferArena + fixed-buffer ops
// -------------------------------------------------------------------------------------

TEST(RegisteredTest, BufferArenaFixedWriteRead)
{
    coop::CooperatorConfiguration cfg;
    cfg.uring.registeredBufferBytes = 1 << 20;

    coop::Cooperator cooperator(cfg);
    coop::Thread thread(&cooperator);

    cooperator.SubmitSync([&](coop::Context*)
    {
        auto* uring = coop::GetUring();
        auto* arena = uring->GetBufferArena();
        ASSERT_NE(arena, nullptr) << "arena registration failed (RLIMIT_MEMLOCK?)";
        ASSERT_TRUE(arena->Available());

        char* lease = arena->Acquire(8192);
        ASSERT_NE(lease, nullptr);
        memset(lease, 'x', 8192);
        memcpy(lease, "fixed-path", 10);

        char tmpPath[] = "/tmp/coop_fixed_XXXXXX";
        int fileFd = mkstemp(tmpPath);
        ASSERT_GE(fileFd, 0);
        unlink(tmpPath);

        coop::io::Descriptor file(coop::io::borrowed, fileFd, uring);

        // WRITE_FIXED via the overload — same op name, fixed-ness from the buffer type
        //
        coop::io::FixedBuffer fb{lease, arena->Index()};
        int written = coop::io::Write(file, fb, 8192, 0);
        ASSERT_EQ(written, 8192);

        // READ_FIXED back into a second lease
        //
        char* lease2 = arena->Acquire(8192);
        ASSERT_NE(lease2, nullptr);
        coop::io::FixedBuffer fb2{lease2, arena->Index()};
        int readBack = coop::io::Read(file, fb2, 8192, 0);
        ASSERT_EQ(readBack, 8192);
        EXPECT_EQ(memcmp(lease2, lease, 8192), 0);

        arena->Release(lease2, 8192);
        arena->Release(lease, 8192);
        ::close(fileFd);

        cooperator.Shutdown();
    });
}

TEST(RegisteredTest, NoArenaWithoutConfig)
{
    test::RunInCooperator([](coop::Context*)
    {
        // Default configuration: no arena, and that is a clean, queryable state
        //
        EXPECT_EQ(coop::GetUring()->GetBufferArena(), nullptr);
    });
}

// -------------------------------------------------------------------------------------
// SendZC: two-CQE zero-copy send (result + F_NOTIF), plain and arena-fixed
// -------------------------------------------------------------------------------------

TEST(RegisteredTest, SendZCRoundtrip)
{
    test::RunInCooperator([](coop::Context*)
    {
        TcpPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor sender(coop::io::borrowed, sp.fds[0], uring);
        coop::io::Descriptor peer(coop::io::borrowed, sp.fds[1], uring);

        // Loopback silently copies (no real zero copy), but the two-CQE lifecycle —
        // and the F_NOTIF result-preservation guard in Handle::Complete — is identical.
        //
        std::vector<char> payload(8192, 'z');
        int sent = coop::io::SendZC(sender, payload.data(), payload.size());
        ASSERT_EQ(sent, (int)payload.size());

        std::vector<char> got(payload.size());
        size_t total = 0;
        while (total < got.size())
        {
            int n = coop::io::Recv(peer, got.data() + total, got.size() - total);
            ASSERT_GT(n, 0);
            total += static_cast<size_t>(n);
        }
        EXPECT_EQ(memcmp(got.data(), payload.data(), payload.size()), 0);
    });
}

TEST(RegisteredTest, SendZCFixedFromArena)
{
    coop::CooperatorConfiguration cfg;
    cfg.uring.registeredBufferBytes = 1 << 20;

    coop::Cooperator cooperator(cfg);
    coop::Thread thread(&cooperator);

    cooperator.SubmitSync([&](coop::Context*)
    {
        auto* uring = coop::GetUring();
        auto* arena = uring->GetBufferArena();
        ASSERT_NE(arena, nullptr);

        TcpPair sp;
        coop::io::Descriptor sender(coop::io::borrowed, sp.fds[0], uring);
        coop::io::Descriptor peer(coop::io::borrowed, sp.fds[1], uring);

        char* lease = arena->Acquire(4096);
        ASSERT_NE(lease, nullptr);
        memset(lease, 'f', 4096);

        coop::io::FixedBuffer fb{lease, arena->Index()};
        int sent = coop::io::SendZC(sender, fb, 4096);
        ASSERT_EQ(sent, 4096);

        std::vector<char> got(4096);
        size_t total = 0;
        while (total < got.size())
        {
            int n = coop::io::Recv(peer, got.data() + total, got.size() - total);
            ASSERT_GT(n, 0);
            total += static_cast<size_t>(n);
        }
        EXPECT_EQ(memcmp(got.data(), lease, 4096), 0);

        arena->Release(lease, 4096);
        cooperator.Shutdown();
    });
}

// A failed zero-copy send (AF_UNIX is unsupported) posts a single CQE with no
// notification — the handle must not wait forever for the second one.
//
TEST(RegisteredTest, SendZCFailureDoesNotHang)
{
    test::RunInCooperator([](coop::Context*)
    {
        RawPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor sender(coop::io::borrowed, sp.fds[0], uring);

        char buf[64] = {};
        int ret = coop::io::SendZC(sender, buf, sizeof(buf));
        EXPECT_LT(ret, 0);
    });
}
