#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/thread.h"

#include "coop/io/armed_handle.h"
#include "coop/io/buffer_ring.h"
#include "coop/io/descriptor.h"
#include "coop/io/uring.h"

#include "test_helpers.h"

namespace
{

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

// A round-tripped stream drained through a single armed multishot recv. Each round writes a
// batch and drains exactly that many bytes, recycling each kernel-selected buffer back to the
// pool. A pool far smaller than the total bytes moved proves the recycle path: the same 16
// buffers serve every round, and the kernel never runs dry (enobufs stays 0). Exactly-once
// delivery is checked by reconstructing the full byte stream and comparing it to what was sent.
//
TEST(ArmedHandleTest, MultishotRecvDrainsStreamWithRecycle)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::io::BufferRing br(/*group=*/7, /*entries=*/16, /*bufSize=*/256);
        ASSERT_EQ(br.Register(*coop::GetUring()), 0);

        SocketPair sp;
        coop::io::Descriptor reader(sp.fds[0], coop::GetUring());

        coop::Coordinator coord;
        coop::io::ArmedHandle ah(ctx, reader, &br, &coord);
        ah.Arm();

        const char* kMsg = "0123456789ABCDEF";   // 16 bytes
        const int kMsgLen = 16;
        const int kRounds = 50;
        const int kPerRound = 4;

        std::string expected;
        std::string received;

        for (int r = 0; r < kRounds; r++)
        {
            for (int m = 0; m < kPerRound; m++)
            {
                ASSERT_EQ(::write(sp.fds[1], kMsg, kMsgLen), (ssize_t)kMsgLen);
                expected.append(kMsg, kMsgLen);
            }

            long roundBytes = (long)kPerRound * kMsgLen;
            long got = 0;
            while (got < roundBytes)
            {
                coop::io::ArmedHandle::Chunk c;
                int n = ah.Next(&c);
                ASSERT_GT(n, 0);                     // a data chunk
                received.append(c.data, n);
                got += n;
            }
        }

        EXPECT_EQ(received, expected);               // exactly-once, in order
        EXPECT_EQ(ah.Enobufs(), 0u);                 // recycle kept the pool full
        EXPECT_GE(ah.Delivered(), (uint64_t)kRounds);

        // ah is destroyed here while still armed -- exercises Cancel + Flash drain through the
        // destructor without stranding the held coordinator.
    });
}

// Peer close surfaces as a zero-length terminal chunk; the stream is then finished.
//
TEST(ArmedHandleTest, MultishotRecvReportsEof)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::io::BufferRing br(7, 16, 256);
        ASSERT_EQ(br.Register(*coop::GetUring()), 0);

        SocketPair sp;
        coop::io::Descriptor reader(sp.fds[0], coop::GetUring());

        coop::Coordinator coord;
        coop::io::ArmedHandle ah(ctx, reader, &br, &coord);
        ah.Arm();

        const char* kMsg = "ping";
        ASSERT_EQ(::write(sp.fds[1], kMsg, 4), (ssize_t)4);

        coop::io::ArmedHandle::Chunk c;
        int n = ah.Next(&c);
        ASSERT_EQ(n, 4);
        EXPECT_EQ(memcmp(c.data, kMsg, 4), 0);

        // Close the writer; the multishot delivers a zero-length completion.
        //
        close(sp.fds[1]);
        sp.fds[1] = -1;

        n = ah.Next(&c);
        EXPECT_EQ(n, 0);
        EXPECT_FALSE(ah.Armed());
    });
}

// Calibration (the RED case): with a pool smaller than the in-flight bytes and no recycling, the
// kernel exhausts the pool and the armed recv surfaces -ENOBUFS. This is the negative control
// that proves the enobufs==0 assertion above is real, not green-by-construction.
//
TEST(ArmedHandleTest, PoolExhaustionSurfacesEnobufs)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::io::BufferRing br(7, /*entries=*/4, /*bufSize=*/256);
        ASSERT_EQ(br.Register(*coop::GetUring()), 0);

        SocketPair sp;
        coop::io::Descriptor reader(sp.fds[0], coop::GetUring());

        coop::Coordinator coord;
        coop::io::ArmedHandle ah(ctx, reader, &br, &coord);
        ah.Arm();

        // 1600 bytes against a 4 * 256 = 1024-byte pool. Deliberately do NOT drain (no Next), so
        // no buffer is recycled and the pool must run dry.
        //
        std::string blob(200, 'x');
        for (int i = 0; i < 8; i++)
        {
            ASSERT_EQ(::write(sp.fds[1], blob.data(), blob.size()), (ssize_t)blob.size());
        }

        // Drive the cooperator's Poll without consuming: each yield lets the scheduler submit the
        // multishot and process completions until the pool is exhausted.
        //
        for (int i = 0; i < 20 && ah.Armed(); i++)
        {
            ctx->Yield();
        }

        EXPECT_GT(ah.Enobufs(), 0u);
        EXPECT_FALSE(ah.Armed());
    });
}

// Uring::Init registers a default buffer ring when the configuration asks for one and the kernel
// supports pbuf rings. Exercises the feature-probe wiring end to end: the configured ring is
// reachable via GetBufferRing() and an armed recv that names no explicit ring drains a stream
// through it. (On a kernel without pbuf-ring support GetBufferRing() would be null; this host has
// it, so we assert the registered path.)
//
TEST(ArmedHandleTest, InitRegistersDefaultBufferRing)
{
    coop::CooperatorConfiguration cfg;
    cfg.uring.bufferRingEntries = 16;
    cfg.uring.bufferRingBufSize = 256;
    cfg.uring.bufferRingGroup = 5;

    coop::Cooperator cooperator(cfg);
    coop::Thread thread(&cooperator);

    cooperator.SubmitSync([](coop::Context* ctx)
    {
        coop::io::BufferRing* br = coop::GetUring()->GetBufferRing();
        ASSERT_NE(br, nullptr);
        EXPECT_EQ(br->Group(), 5);
        EXPECT_EQ(br->Entries(), 16u);

        SocketPair sp;
        coop::io::Descriptor reader(sp.fds[0], coop::GetUring());

        coop::Coordinator coord;
        coop::io::ArmedHandle ah(ctx, reader, br, &coord);
        ah.Arm();

        const char* kMsg = "default-ring";
        const int kLen = 12;
        ASSERT_EQ(::write(sp.fds[1], kMsg, kLen), (ssize_t)kLen);

        coop::io::ArmedHandle::Chunk c;
        int n = ah.Next(&c);
        ASSERT_EQ(n, kLen);
        EXPECT_EQ(memcmp(c.data, kMsg, kLen), 0);
    });

    cooperator.Shutdown();
}

} // namespace
