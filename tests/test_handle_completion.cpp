#include <atomic>
#include <netinet/in.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "completion_observer.h"
#include "coop/continuation.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/self.h"
#include "coop/signal.h"
#include "coop/thread.h"

#include "coop/io/descriptor.h"
#include "coop/io/handle.h"
#include "coop/io/recv.h"
#include "coop/io/uring.h"

namespace
{

// C4 callback inventory (Handle::Callback dispatch): one-shot primary/notification and
// secondary timeout/cancel reach Finalize -> Release(..., false), which only queues waiters.
// Armed recv data/terminal/re-arm/cancel-ack enqueue slots, recycle buffers or acquire SQEs;
// Armed accept queue/overflow-close/pause/re-arm/cancel-ack enqueue, close or acquire SQEs.
// Their shared WakeConsumer also uses Release(..., false). Timer expiry clears the armed
// timer state; update acknowledgments are ignored. None calls an application context or drains
// a queued continuation. These tests cover one-shot and secondary paths; the callback suite
// covers armed receive data/EOF, accept pause/cancel/overflow, and scheduler timer expiry.
// The zero-copy test below claims notification coverage only when the observer records both
// the F_MORE primary CQE and F_NOTIF release CQE for its Handle.

struct SocketPair
{
    int fds[2]{-1, -1};

    SocketPair() { EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0); }
    ~SocketPair()
    {
        if (fds[0] >= 0)
            ::close(fds[0]);
        if (fds[1] >= 0)
            ::close(fds[1]);
    }
};

// SEND_ZC is a TCP/UDP operation; AF_UNIX would exercise only its unsupported case.
struct TcpPair
{
    int fds[2]{-1, -1};
    int listener{-1};

    bool Open()
    {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0)
            return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener, 1) != 0)
            return false;
        socklen_t length = sizeof(address);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            return false;
        fds[0] = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fds[0] < 0 || ::connect(fds[0], reinterpret_cast<sockaddr*>(&address), length) != 0)
            return false;
        fds[1] = ::accept(listener, nullptr, nullptr);
        return fds[1] >= 0;
    }

    ~TcpPair()
    {
        if (listener >= 0)
            ::close(listener);
        for (int fd : fds)
            if (fd >= 0)
                ::close(fd);
    }
};

struct ProducerGate
{
    void Open()
    {
        std::lock_guard lock(mutex);
        open = true;
        cv.notify_one();
    }

    void Wait()
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return open; });
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool open{false};
};

uint64_t TestSeed()
{
    auto* value = std::getenv("STRATA_COOP_COMPLETION_SEED");
    if (value && *value)
    {
        char* end = nullptr;
        auto seed = std::strtoull(value, &end, 0);
        if (*end == '\0')
            return seed;
    }
    return std::random_device{}();
}

enum class RingMode
{
    Bare,
    CoopTaskrun,
    DeferTaskrun,
    Sqpoll
};

const char* ModeName(RingMode mode)
{
    switch (mode)
    {
        case RingMode::Bare:
            return "bare";
        case RingMode::CoopTaskrun:
            return "coop_taskrun";
        case RingMode::DeferTaskrun:
            return "defer_taskrun";
        case RingMode::Sqpoll:
            return "sqpoll";
    }
    return "unknown";
}

unsigned RequestedModeFlag(RingMode mode)
{
    switch (mode)
    {
        case RingMode::Bare:
            return 0;
        case RingMode::CoopTaskrun:
            return IORING_SETUP_COOP_TASKRUN;
        case RingMode::DeferTaskrun:
            return IORING_SETUP_DEFER_TASKRUN;
        case RingMode::Sqpoll:
            return IORING_SETUP_SQPOLL;
    }
    return 0;
}

class HandleCompletionTest : public ::testing::TestWithParam<RingMode>
{
};

TEST_P(HandleCompletionTest, ParkedIssuerAndIndependentContinuationDoNotRunInsideDrive)
{
    auto mode = GetParam();
    uint64_t seed = TestSeed();
    std::mt19937_64 random(seed);
    size_t length = 8 + random() % 128;
    bool targetFirst = (random() & 1) != 0;
    std::string payload(length, '\0');
    for (char& c : payload)
        c = static_cast<char>('a' + random() % 26);
    SCOPED_TRACE(::testing::Message() << "seed=" << seed << " mode=" << ModeName(mode));

    coop::CooperatorConfiguration config;
    config.uring.coopTaskrun = mode == RingMode::CoopTaskrun;
    config.uring.deferTaskrun = mode == RingMode::DeferTaskrun;
    config.uring.sqpoll = mode == RingMode::Sqpoll;
    if (config.uring.sqpoll)
        config.uring.entries = 1024;

    unsigned actualFlags = 0;
    {
        coop::Cooperator cooperator(config);
        coop::Thread thread(&cooperator);
        ASSERT_TRUE(cooperator.SubmitSync(
            [&](coop::Context* issuer)
            {
                auto* ring = issuer->GetCooperator()->GetUring();
                actualFlags = ring->Ring()->flags;
                unsigned requested = RequestedModeFlag(mode);
                unsigned modeFlags =
                    IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SQPOLL;
                if ((actualFlags & modeFlags) != requested)
                {
                    issuer->GetCooperator()->Shutdown();
                    return;
                }

                SocketPair targetSockets;
                SocketPair otherSockets;
                coop::io::Descriptor targetReader(coop::io::borrowed, targetSockets.fds[0], ring);
                coop::io::Descriptor otherReader(coop::io::borrowed, otherSockets.fds[0], ring);
                coop::Coordinator targetCoord;
                coop::Coordinator otherCoord;
                coop::io::Handle target(issuer, targetReader, &targetCoord);
                coop::io::Handle other(issuer, otherReader, &otherCoord);
                std::vector<char> targetBuf(length);
                char otherBuf[8]{};
                int baseline = ring->PendingOps();
                EXPECT_TRUE(
                    coop::io::Recv(target, targetBuf.data(), targetBuf.size(), MSG_WAITALL));
                EXPECT_TRUE(coop::io::Recv(other, otherBuf, sizeof(otherBuf)));
                EXPECT_EQ(ring->PendingOps(), baseline + 2);

                int continuationRuns = 0;
                auto continuation = otherCoord.Continue(
                    [&](coop::Coordinator*)
                    {
                        ++continuationRuns;
                        EXPECT_EQ(other.Result(), 1);
                        EXPECT_EQ(otherBuf[0], '!');
                        return 0;
                    });

                ProducerGate gate;
                std::atomic<ssize_t> otherWrite{-1};
                std::atomic<ssize_t> targetWrite{-1};
                std::thread producer(
                    [&]
                    {
                        gate.Wait();
                        auto sendOther = [&]
                        {
                            otherWrite = ::send(otherSockets.fds[1], "!", 1, MSG_NOSIGNAL);
                            ::shutdown(otherSockets.fds[1], SHUT_WR);
                        };
                        auto sendTarget = [&]
                        {
                            targetWrite = ::send(targetSockets.fds[1], payload.data(),
                                                 payload.size(), MSG_NOSIGNAL);
                            ::shutdown(targetSockets.fds[1], SHUT_WR);
                        };
                        if (targetFirst)
                        {
                            sendTarget();
                            sendOther();
                        }
                        else
                        {
                            sendOther();
                            sendTarget();
                        }
                    });

                coop::Signal driverDone(issuer);
                bool issuerWaiting = false;
                bool issuerResumed = false;
                int siblingRuns = 0;
                int driverResult = -1;
                bool spawned = issuer->GetCooperator()->Spawn(
                    [&](coop::Context* sibling)
                    {
                        // Let the issuer enter ordinary Wait before this sibling starts the drive.
                        sibling->Yield(true);
                        EXPECT_TRUE(issuerWaiting);
                        gate.Open();
                        {
                            coop::test::CompletionObserver observer;
                            EXPECT_EQ(other.WaitWithoutYield(), 1);
                            driverResult = target.WaitWithoutYield();
                            EXPECT_FALSE(observer.overflow);
                            EXPECT_EQ(observer.Count(reinterpret_cast<uintptr_t>(&other)), 1);
                            EXPECT_EQ(observer.Count(reinterpret_cast<uintptr_t>(&target)), 1);
                        }
                        EXPECT_EQ(driverResult, static_cast<int>(length));
                        // The runtime's other operations can change the ring-wide count while
                        // the issuer parks. Prove this drain through these two Handles.
                        EXPECT_EQ(target.Result(), driverResult);
                        EXPECT_EQ(other.Result(), 1);
                        EXPECT_EQ(std::memcmp(targetBuf.data(), payload.data(), length), 0);
                        EXPECT_FALSE(issuerResumed)
                            << "NO_YIELD: issuer ran inside completion drive";
                        EXPECT_EQ(continuationRuns, 0)
                            << "NO_YIELD: continuation ran inside completion drive";
                        sibling->Yield(true);
                        ++siblingRuns;
                        driverDone.Notify(sibling, false);
                    });

                if (!spawned)
                {
                    ADD_FAILURE() << "could not spawn completion driver";
                    gate.Open();
                    EXPECT_EQ(other.WaitWithoutYield(), 1);
                    EXPECT_EQ(target.WaitWithoutYield(), static_cast<int>(length));
                    EXPECT_EQ(continuation.Await(), 0);
                    EXPECT_EQ(continuationRuns, 1);
                    producer.join();
                    EXPECT_EQ(otherWrite.load(), 1);
                    EXPECT_EQ(targetWrite.load(), static_cast<ssize_t>(length));
                    EXPECT_EQ(std::memcmp(targetBuf.data(), payload.data(), length), 0);
                    issuer->GetCooperator()->Shutdown();
                    return;
                }

                issuerWaiting = true;
                int issuerResult = target.Wait();
                issuerResumed = true;
                driverDone.Wait(issuer);
                producer.join();
                EXPECT_EQ(otherWrite.load(), 1);
                EXPECT_EQ(targetWrite.load(), static_cast<ssize_t>(length));
                EXPECT_EQ(issuerResult, driverResult);
                EXPECT_EQ(issuerResult, static_cast<int>(length));
                EXPECT_EQ(continuation.Await(), 0);
                EXPECT_EQ(continuationRuns, 1);
                EXPECT_EQ(siblingRuns, 1);
                issuer->GetCooperator()->Shutdown();
            }));
    }
    if ((actualFlags & (IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN |
                        IORING_SETUP_SQPOLL)) != RequestedModeFlag(mode))
    {
        GTEST_SKIP() << "requested " << ModeName(mode) << " unavailable; actual ring flags=0x"
                     << std::hex << actualFlags << " (coverage gap)";
    }
    RecordProperty("actual_ring_flags", static_cast<int>(actualFlags));
}

INSTANTIATE_TEST_SUITE_P(RingModes, HandleCompletionTest,
                         ::testing::Values(RingMode::Bare, RingMode::CoopTaskrun,
                                           RingMode::DeferTaskrun, RingMode::Sqpoll),
                         [](auto const& info) { return ModeName(info.param); });

#ifndef NDEBUG
TEST(HandleCompletionDeathTest, ContinuationCannotDriveRingFromApplicationContext)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH(
        {
            coop::Cooperator cooperator;
            coop::Thread thread(&cooperator);
            cooperator.SubmitSync(
                [&](coop::Context* ctx)
                {
                    auto* ring = ctx->GetCooperator()->GetUring();
                    coop::io::Descriptor invalid(coop::io::borrowed, -1, ring);
                    coop::Coordinator completion;
                    coop::io::Handle handle(ctx, invalid, &completion);
                    char byte{};
                    coop::io::Recv(handle, &byte, 1);

                    // Explicit draining borrows the calling application context. Self() and
                    // native-ring checks therefore pass, but the thunk guard must reject it.
                    coop::Coordinator trigger;
                    trigger.Acquire(ctx);
                    trigger.ContinueDetached(
                        [&](coop::Coordinator*) { handle.WaitWithoutYield(); });
                    trigger.Release(ctx, false);
                    cooperator.DrainContinuations();
                    cooperator.Shutdown();
                });
        },
        "must not suspend");
}
#endif

TEST(HandleCompletionEdgeTest, IssuerCanDriveErrorCancellationAndTimeout)
{
    uint64_t seed = TestSeed();
    int errorOps = 1 + seed % 3;
    SCOPED_TRACE(::testing::Message() << "seed=" << seed << " error_ops=" << errorOps);
    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);
    ASSERT_TRUE(cooperator.SubmitSync(
        [&](coop::Context* ctx)
        {
            auto* ring = ctx->GetCooperator()->GetUring();
            int baseline = ring->PendingOps();
            char buf[8]{};

            // The closed descriptor produces an ordinary negative primary result. Its second
            // wait returns the cached result without entering the ring or changing accounting.
            coop::io::Descriptor invalid(coop::io::borrowed, -1, ring);
            for (int i = 0; i < errorOps; ++i)
            {
                coop::Coordinator coord;
                coop::io::Handle handle(ctx, invalid, &coord);
                EXPECT_TRUE(coop::io::Recv(handle, buf, sizeof(buf)));
                EXPECT_EQ(handle.WaitWithoutYield(), -EBADF);
                EXPECT_EQ(ring->PendingOps(), baseline);
                EXPECT_EQ(handle.WaitWithoutYield(), -EBADF);
                EXPECT_EQ(ring->PendingOps(), baseline);
            }

            SocketPair sockets;
            coop::io::Descriptor reader(coop::io::borrowed, sockets.fds[0], ring);
            {
                coop::Coordinator coord;
                coop::io::Handle handle(ctx, reader, &coord);
                EXPECT_TRUE(coop::io::Recv(handle, buf, sizeof(buf)));
                handle.Cancel();
                EXPECT_EQ(handle.WaitWithoutYield(), -ECANCELED);
                EXPECT_EQ(ring->PendingOps(), baseline);
            }
            {
                coop::Coordinator coord;
                coop::io::Handle handle(ctx, reader, &coord);
                EXPECT_TRUE(
                    coop::io::Recv(handle, buf, sizeof(buf), 0, std::chrono::milliseconds(1)));
                EXPECT_EQ(handle.WaitWithoutYield(), -ECANCELED);
                EXPECT_TRUE(handle.TimedOut());
                EXPECT_EQ(ring->PendingOps(), baseline);
            }
            ctx->GetCooperator()->Shutdown();
        }));
}

TEST(HandleCompletionEdgeTest, ZeroCopyPrimaryAndNotificationDrainWhenSupported)
{
    int result = 0;
    int pendingAfter = -1;
    int received = 0;
    bool notificationObserved = false;
    std::string receivedPayload(8192, '\0');
    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);
    ASSERT_TRUE(cooperator.SubmitSync(
        [&](coop::Context* ctx)
        {
            auto* ring = ctx->GetCooperator()->GetUring();
            TcpPair sockets;
            if (!sockets.Open())
            {
                ADD_FAILURE() << "could not create local TCP pair";
                ctx->GetCooperator()->Shutdown();
                return;
            }
            coop::io::Descriptor writer(coop::io::borrowed, sockets.fds[0], ring);
            coop::Coordinator coord;
            coop::io::Handle handle(ctx, writer, &coord);
            std::string payload(8192, 'z');
            std::thread consumer(
                [&]
                {
                    while (received < static_cast<int>(receivedPayload.size()))
                    {
                        auto n = ::read(sockets.fds[1], receivedPayload.data() + received,
                                        receivedPayload.size() - received);
                        if (n <= 0)
                            break;
                        received += static_cast<int>(n);
                    }
                });
            auto* sqe = ring->GetSqe();
            EXPECT_NE(sqe, nullptr);
            if (sqe)
            {
                io_uring_prep_send_zc(sqe, writer.m_fd, payload.data(), payload.size(), 0, 0);
                int before = ring->PendingOps();
                handle.Submit(sqe, 2);
                {
                    coop::test::CompletionObserver observer;
                    result = handle.WaitWithoutYield();
                    auto data = reinterpret_cast<uintptr_t>(&handle);
                    auto primaryCount = observer.Count(data, IORING_CQE_F_NOTIF, 0);
                    auto primaryMoreCount = observer.Count(
                        data, IORING_CQE_F_MORE | IORING_CQE_F_NOTIF, IORING_CQE_F_MORE);
                    auto notificationCount =
                        observer.Count(data, IORING_CQE_F_NOTIF, IORING_CQE_F_NOTIF);
                    auto handleCount = observer.Count(data);
                    EXPECT_FALSE(observer.overflow);
                    EXPECT_EQ(primaryCount, 1);
                    if (notificationCount != 0)
                    {
                        EXPECT_EQ(primaryMoreCount, 1);
                        EXPECT_EQ(notificationCount, 1);
                        EXPECT_EQ(handleCount, 2);
                    }
                    notificationObserved = !observer.overflow && primaryCount == 1 &&
                                           primaryMoreCount == 1 && notificationCount == 1 &&
                                           handleCount == 2;
                }
                pendingAfter = ring->PendingOps() - before;
                EXPECT_EQ(handle.Result(), result);
                EXPECT_EQ(pendingAfter, 0);
                if (result >= 0 && notificationObserved)
                {
                    // Observed primary and buffer-release CQEs precede Handle reuse. The linked
                    // timeout must drain both of its CQEs and preserve its primary result.
                    char byte{};
                    EXPECT_TRUE(coop::io::Recv(handle, &byte, 1, 0, std::chrono::milliseconds(1)));
                    EXPECT_EQ(handle.WaitWithoutYield(), -ECANCELED);
                    EXPECT_TRUE(handle.TimedOut());
                    EXPECT_EQ(ring->PendingOps(), before);
                }
            }
            ::shutdown(sockets.fds[0], SHUT_WR);
            consumer.join();
            ctx->GetCooperator()->Shutdown();
        }));
    if (result == -EOPNOTSUPP || result == -EINVAL)
    {
        GTEST_SKIP() << "IORING_OP_SEND_ZC unavailable for TCP socket (coverage gap): " << result;
    }
    EXPECT_EQ(result, 8192);
    EXPECT_EQ(pendingAfter, 0);
    EXPECT_EQ(received, 8192);
    EXPECT_EQ(receivedPayload, std::string(8192, 'z'));
    if (result >= 0 && !notificationObserved)
    {
        GTEST_SKIP() << "SEND_ZC returned a positive result without observed F_MORE primary and "
                        "F_NOTIF release CQEs for this Handle (notification coverage gap)";
    }
}

} // end anonymous namespace
