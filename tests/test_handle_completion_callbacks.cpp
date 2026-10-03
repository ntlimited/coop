#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <liburing.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/continuation.h"
#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/coordinator.h"
#include "coop/detail/timer_tag.h"
#include "coop/signal.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"

#include "coop/io/armed_accept.h"
#include "coop/io/armed_handle.h"
#include "coop/io/buffer_ring.h"
#include "coop/io/descriptor.h"
#include "coop/io/handle.h"
#include "coop/io/recv.h"
#include "coop/io/timeout.h"
#include "coop/io/uring.h"

#include "completion_observer.h"

namespace
{

struct SocketPair
{
    int fd[2]{-1, -1};

    SocketPair() { EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd), 0); }
    ~SocketPair()
    {
        if (fd[0] >= 0)
            ::close(fd[0]);
        if (fd[1] >= 0)
            ::close(fd[1]);
    }
};

// Keep target completion behind observed native dispatch, rather than relying on
// elapsed time to order unrelated CQEs. A timeout still lets the target complete so
// the missing dispatch is reported by the test's assertions.
//
struct CompletionGate
{
    static constexpr unsigned kData = 1;
    static constexpr unsigned kTerminal = 2;
    static constexpr unsigned kTimeout = 4;
    static constexpr unsigned kReturned = 8;
    static constexpr unsigned kConsumed = 16;

    uintptr_t armedData{0};
    uintptr_t timeoutData{0};
    std::mutex mutex;
    std::condition_variable changed;
    unsigned observed{0};

    void Publish(unsigned bits)
    {
        std::lock_guard lock(mutex);
        observed |= bits;
        changed.notify_one();
    }

    bool Wait(unsigned bits)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(2),
                                [&] { return (observed & bits) == bits; });
    }

    static void AfterDispatch(const coop::test::CompletionRecord& cqe, void* data)
    {
        auto& gate = *static_cast<CompletionGate*>(data);
        unsigned bits = 0;
        if (cqe.data == gate.armedData)
        {
            if (cqe.result > 0 && (cqe.flags & IORING_CQE_F_BUFFER))
                bits |= kData;
            if (cqe.result == 0 && !(cqe.flags & IORING_CQE_F_MORE))
                bits |= kTerminal;
        }
        if (cqe.data == gate.timeoutData && cqe.result == -ETIME)
            bits |= kTimeout;
        if (bits)
            gate.Publish(bits);
    }
};

// A kernel timer gates the target writer. The earlier, unrelated timeout and socket
// completion can therefore be harvested during the target drive without relying on a
// same-cooperator producer context or a scheduler yield.
//
bool WaitForTimer(std::chrono::milliseconds delay)
{
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (fd < 0)
        return false;
    itimerspec spec{};
    spec.it_value.tv_sec = delay.count() / 1000;
    spec.it_value.tv_nsec = (delay.count() % 1000) * 1000000;
    bool ok = ::timerfd_settime(fd, 0, &spec, nullptr) == 0;
    uint64_t expirations = 0;
    if (ok)
        ok = ::read(fd, &expirations, sizeof(expirations)) == sizeof(expirations);
    ::close(fd);
    return ok && expirations == 1;
}

struct Listener
{
    int fd{-1};

    Listener()
    {
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        EXPECT_GE(fd, 0);
        if (fd < 0)
            return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        EXPECT_EQ(::listen(fd, 64), 0);
    }

    ~Listener()
    {
        if (fd >= 0)
            ::close(fd);
    }

    int Connect(bool nonblocking = true) const
    {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            return -1;
        int client = ::socket(AF_INET, SOCK_STREAM | (nonblocking ? SOCK_NONBLOCK : 0), 0);
        if (client < 0)
            return -1;
        if (::connect(client, reinterpret_cast<sockaddr*>(&address), length) != 0 &&
            (!nonblocking || errno != EINPROGRESS))
        {
            ::close(client);
            return -1;
        }
        return client;
    }
};

TEST(HandleCompletionCallbacksTest, ArmedReceiveAndTimeoutStayDeferredThroughDrive)
{
    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);
    ASSERT_TRUE(cooperator.SubmitSync(
        [&](coop::Context* driver)
        {
            auto* ring = driver->GetCooperator()->GetUring();
            coop::io::BufferRing buffers(7, 16, 256);
            if (buffers.Register(*ring) != 0)
            {
                ADD_FAILURE() << "provided-buffer registration failed";
                driver->GetCooperator()->Shutdown();
                return;
            }

            SocketPair otherSockets;
            SocketPair targetSockets;
            bool receiverReady = false;
            bool receiverDone = false;
            int receiverRuns = 0;
            int terminal = -1;
            std::string captured;
            uint64_t deliveredAfter = 0;
            coop::io::ArmedHandle* armed = nullptr;

            bool spawned = driver->GetCooperator()->Spawn(
                [&](coop::Context* receiver)
                {
                    {
                        coop::io::Descriptor descriptor(coop::io::borrowed, otherSockets.fd[0],
                                                        ring);
                        coop::Coordinator coordinator;
                        coop::io::ArmedHandle handle(receiver, descriptor, &buffers, &coordinator);
                        armed = &handle;
                        handle.Arm();
                        receiverReady = true;
                        coop::io::ArmedHandle::Chunk chunk{};
                        do
                        {
                            terminal = handle.Next(&chunk);
                            if (terminal > 0)
                                captured.append(chunk.data, terminal);
                        } while (terminal > 0);
                        ++receiverRuns;
                        deliveredAfter = handle.Delivered();
                    }
                    armed = nullptr;
                    receiverDone = true;
                });
            if (!spawned)
            {
                ADD_FAILURE() << "receiver context did not spawn";
                driver->GetCooperator()->Shutdown();
                return;
            }
            while (!receiverReady)
                driver->Yield(true);

            coop::Coordinator timeoutCoordinator;
            coop::io::Handle timeout(driver, ring, &timeoutCoordinator);
            if (!coop::io::Timeout(timeout, std::chrono::milliseconds(1)))
            {
                ADD_FAILURE() << "unrelated timeout did not submit";
                ::write(otherSockets.fd[1], "data", 4);
                ::close(otherSockets.fd[1]);
                otherSockets.fd[1] = -1;
                while (!receiverDone)
                    driver->Yield(true);
                driver->GetCooperator()->Shutdown();
                return;
            }
            int continuationRuns = 0;
            auto continuation = timeoutCoordinator.Continue(
                [&](coop::Coordinator*)
                {
                    ++continuationRuns;
                    EXPECT_EQ(timeout.Result(), -ETIME);
                    return 0;
                });

            coop::io::Descriptor targetDescriptor(coop::io::borrowed, targetSockets.fd[0], ring);
            coop::Coordinator targetCoordinator;
            coop::io::Handle target(driver, targetDescriptor, &targetCoordinator);
            char targetByte{};
            EXPECT_TRUE(coop::io::Recv(target, &targetByte, 1));
            CompletionGate gate;
            gate.armedData = reinterpret_cast<uintptr_t>(armed) | uintptr_t(0x2);
            gate.timeoutData = reinterpret_cast<uintptr_t>(&timeout);
            bool producerOk = false;
            std::thread producer(
                [&]
                {
                    bool ok = ::write(otherSockets.fd[1], "data", 4) == 4;
                    // Linux 6.17 can lose the AF_UNIX multishot EOF when data and close
                    // wakeups coalesce (liburing issue #1549). Dispatch the data before
                    // closing so this tests callback deferral independently of that bug.
                    // https://github.com/torvalds/linux/commit/a68ed2df7213
                    //
                    bool dataDispatched = gate.Wait(CompletionGate::kData);
                    ::close(otherSockets.fd[1]);
                    otherSockets.fd[1] = -1;
                    bool dispatched = gate.Wait(CompletionGate::kData | CompletionGate::kTerminal |
                                                CompletionGate::kTimeout);
                    bool targetOk = ::write(targetSockets.fd[1], "!", 1) == 1;
                    ::shutdown(targetSockets.fd[1], SHUT_WR);
                    producerOk = ok && dataDispatched && dispatched && targetOk;
                });

            {
                coop::test::CompletionObserver completions;
                completions.afterDispatch = CompletionGate::AfterDispatch;
                completions.afterDispatchData = &gate;
                int result = target.WaitWithoutYield();
                EXPECT_EQ(result, 1);
                EXPECT_EQ(targetByte, '!');
                EXPECT_NE(armed, nullptr);
                if (armed)
                {
                    EXPECT_GE(armed->Delivered(), 1u) << "armed data CQE was not harvested";
                    EXPECT_FALSE(armed->Armed()) << "armed terminal CQE was not harvested";
                    auto data = reinterpret_cast<uintptr_t>(armed) | uintptr_t(0x2);
                    bool sawData = false;
                    bool sawTerminal = false;
                    for (size_t i = 0; i < completions.size; ++i)
                    {
                        const auto& cqe = completions.records[i];
                        sawData |=
                            cqe.data == data && cqe.result > 0 && (cqe.flags & IORING_CQE_F_BUFFER);
                        sawTerminal |=
                            cqe.data == data && cqe.result == 0 && !(cqe.flags & IORING_CQE_F_MORE);
                    }
                    EXPECT_TRUE(sawData) << "armed receive data CQE was not observed during drive";
                    EXPECT_TRUE(sawTerminal)
                        << "armed receive EOF CQE was not observed during drive";
                }
                EXPECT_FALSE(completions.overflow);
                if (completions.Count(gate.timeoutData) > 0)
                    EXPECT_EQ(timeout.Result(), -ETIME);
                else
                    ADD_FAILURE() << "timer CQE was not harvested";
                EXPECT_EQ(receiverRuns, 0) << "NO_YIELD: armed receiver ran inside drive";
                EXPECT_EQ(continuationRuns, 0) << "NO_YIELD: continuation ran inside drive";
            }

            producer.join();
            EXPECT_TRUE(producerOk);
            while (!receiverDone || continuationRuns == 0)
                driver->Yield(true);
            EXPECT_EQ(receiverRuns, 1);
            EXPECT_EQ(captured, "data");
            EXPECT_EQ(terminal, 0);
            EXPECT_GE(deliveredAfter, 1u);
            EXPECT_EQ(continuationRuns, 1);
            EXPECT_EQ(continuation.Await(), 0);
            driver->GetCooperator()->Shutdown();
        }));
}

TEST(HandleCompletionCallbacksTest, ArmedReceiveEofAfterDriveWakesConsumer)
{
    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);
    ASSERT_TRUE(cooperator.SubmitSync(
        [&](coop::Context* driver)
        {
            auto* ring = driver->GetCooperator()->GetUring();
            coop::io::BufferRing buffers(7, 16, 256);
            if (buffers.Register(*ring) != 0)
            {
                ADD_FAILURE() << "provided-buffer registration failed";
                driver->GetCooperator()->Shutdown();
                return;
            }

            SocketPair otherSockets;
            SocketPair targetSockets;
            CompletionGate gate;
            bool receiverReady = false;
            bool receiverDone = false;
            int receiverRuns = 0;
            int terminal = -1;
            std::string captured;
            coop::io::ArmedHandle* armed = nullptr;
            bool spawned = driver->GetCooperator()->Spawn(
                [&](coop::Context* receiver)
                {
                    {
                        coop::io::Descriptor descriptor(coop::io::borrowed, otherSockets.fd[0],
                                                        ring);
                        coop::Coordinator coordinator;
                        coop::io::ArmedHandle handle(receiver, descriptor, &buffers, &coordinator);
                        armed = &handle;
                        handle.Arm();
                        receiverReady = true;
                        coop::io::ArmedHandle::Chunk chunk{};
                        do
                        {
                            terminal = handle.Next(&chunk);
                            if (terminal > 0)
                            {
                                captured.append(chunk.data, terminal);
                                ++receiverRuns;
                                gate.Publish(CompletionGate::kConsumed);
                            }
                        } while (terminal > 0);
                    }
                    armed = nullptr;
                    receiverDone = true;
                });
            if (!spawned)
            {
                ADD_FAILURE() << "receiver context did not spawn";
                driver->GetCooperator()->Shutdown();
                return;
            }
            while (!receiverReady)
                driver->Yield(true);
            gate.armedData = reinterpret_cast<uintptr_t>(armed) | uintptr_t(0x2);

            coop::io::Descriptor targetDescriptor(coop::io::borrowed, targetSockets.fd[0], ring);
            coop::Coordinator targetCoordinator;
            coop::io::Handle target(driver, targetDescriptor, &targetCoordinator);
            char targetByte{};
            EXPECT_TRUE(coop::io::Recv(target, &targetByte, 1));
            bool producerOk = false;
            std::thread producer(
                [&]
                {
                    bool dataOk = ::write(otherSockets.fd[1], "data", 4) == 4;
                    bool dispatched = gate.Wait(CompletionGate::kData);
                    bool targetOk = ::write(targetSockets.fd[1], "!", 1) == 1;
                    ::shutdown(targetSockets.fd[1], SHUT_WR);
                    // The first chunk is consumed on the ordinary scheduler after the
                    // drive returns; only then deliver EOF to its next parked Next().
                    //
                    bool consumed =
                        gate.Wait(CompletionGate::kReturned | CompletionGate::kConsumed);
                    ::close(otherSockets.fd[1]);
                    otherSockets.fd[1] = -1;
                    producerOk = dataOk && dispatched && targetOk && consumed;
                });
            {
                coop::test::CompletionObserver completions;
                completions.afterDispatch = CompletionGate::AfterDispatch;
                completions.afterDispatchData = &gate;
                EXPECT_EQ(target.WaitWithoutYield(), 1);
                EXPECT_EQ(targetByte, '!');
                EXPECT_EQ(receiverRuns, 0) << "NO_YIELD: armed receiver ran inside drive";
                EXPECT_NE(armed, nullptr);
                if (armed)
                {
                    EXPECT_GE(armed->Delivered(), 1u);
                    EXPECT_TRUE(armed->Armed()) << "peer must stay open through the drive";
                }
                EXPECT_FALSE(completions.overflow);
            }
            gate.Publish(CompletionGate::kReturned);
            while (!receiverDone)
                driver->Yield(true);
            producer.join();
            EXPECT_TRUE(producerOk);
            EXPECT_EQ(receiverRuns, 1);
            EXPECT_EQ(captured, "data");
            EXPECT_EQ(terminal, 0);
            driver->GetCooperator()->Shutdown();
        }));
}

TEST(HandleCompletionCallbacksTest, ArmedAcceptPauseAndResumeKeepConsumerDeferred)
{
    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);
    ASSERT_TRUE(cooperator.SubmitSync(
        [&](coop::Context* driver)
        {
            auto* ring = driver->GetCooperator()->GetUring();
            Listener listener;
            SocketPair targetSockets;
            bool acceptReady = false;
            bool firstDone = false;
            bool acceptDone = false;
            int acceptRuns = 0;
            int firstFd = -1;
            int secondFd = -1;
            uint64_t deliveredAfter = 0;
            coop::io::ArmedAccept* armed = nullptr;
            coop::Signal secondConnection(driver);
            coop::Context::Handle consumerContext;

            bool spawned = driver->GetCooperator()->Spawn(
                [&](coop::Context* consumer)
                {
                    {
                        coop::io::Descriptor descriptor(coop::io::borrowed, listener.fd, ring);
                        coop::Coordinator coordinator;
                        coop::io::ArmedAccept handle(consumer, descriptor, &coordinator, 1);
                        armed = &handle;
                        handle.Arm();
                        acceptReady = true;
                        firstFd = handle.Next();
                        ++acceptRuns;
                        if (firstFd >= 0)
                            ::close(firstFd);
                        firstDone = true;
                        secondConnection.Wait(consumer);
                        secondFd = handle.Next();
                        ++acceptRuns;
                        if (secondFd >= 0)
                            ::close(secondFd);
                        deliveredAfter = handle.Delivered();
                    }
                    armed = nullptr;
                    acceptDone = true;
                },
                &consumerContext);
            if (!spawned)
            {
                ADD_FAILURE() << "accept context did not spawn";
                driver->GetCooperator()->Shutdown();
                return;
            }
            while (!acceptReady)
                driver->Yield(true);

            coop::io::Descriptor targetDescriptor(coop::io::borrowed, targetSockets.fd[0], ring);
            coop::Coordinator targetCoordinator;
            coop::io::Handle target(driver, targetDescriptor, &targetCoordinator);
            char targetByte{};
            EXPECT_TRUE(coop::io::Recv(target, &targetByte, 1));
            int firstClient = -1;
            bool producerOk = false;
            std::thread producer(
                [&]
                {
                    firstClient = listener.Connect();
                    bool timerOk = WaitForTimer(std::chrono::milliseconds(20));
                    bool targetOk = ::write(targetSockets.fd[1], "!", 1) == 1;
                    ::shutdown(targetSockets.fd[1], SHUT_WR);
                    producerOk = firstClient >= 0 && timerOk && targetOk;
                });

            {
                coop::test::CompletionObserver completions;
                int result = target.WaitWithoutYield();
                EXPECT_EQ(result, 1);
                EXPECT_EQ(targetByte, '!');
                EXPECT_NE(armed, nullptr);
                if (armed)
                {
                    EXPECT_EQ(armed->Delivered(), 1u) << "accept CQE was not harvested";
                    EXPECT_GE(armed->Paused(), 1u) << "accept pause was not harvested";
                    EXPECT_FALSE(armed->Armed()) << "accept cancel terminal was not harvested";
                    auto data = reinterpret_cast<uintptr_t>(armed) | uintptr_t(0x6);
                    EXPECT_GE(completions.Count(data | uintptr_t(0x1), IORING_CQE_F_MORE, 0), 1u)
                        << "accept cancel acknowledgment was not observed during drive";
                    bool sawTerminal = false;
                    for (size_t i = 0; i < completions.size; ++i)
                    {
                        const auto& cqe = completions.records[i];
                        sawTerminal |= cqe.data == data && cqe.result == -ECANCELED &&
                                       !(cqe.flags & IORING_CQE_F_MORE);
                    }
                    EXPECT_TRUE(sawTerminal)
                        << "accept cancel terminal was not observed during drive";
                }
                EXPECT_FALSE(completions.overflow);
                EXPECT_EQ(acceptRuns, 0) << "NO_YIELD: accept consumer ran inside drive";
            }

            producer.join();
            EXPECT_TRUE(producerOk);
            if (firstClient < 0)
                consumerContext.Kill();
            while (!firstDone)
                driver->Yield(true);
            EXPECT_EQ(acceptRuns, 1);
            int secondClient = listener.Connect();
            EXPECT_GE(secondClient, 0);
            if (secondClient < 0)
                consumerContext.Kill();
            secondConnection.Notify(driver, false);
            while (!acceptDone)
                driver->Yield(true);
            EXPECT_EQ(acceptRuns, 2);
            EXPECT_GE(firstFd, 0);
            EXPECT_GE(secondFd, 0);
            EXPECT_EQ(deliveredAfter, 2u);
            if (firstClient >= 0)
                ::close(firstClient);
            if (secondClient >= 0)
                ::close(secondClient);
            driver->GetCooperator()->Shutdown();
        }));
}

TEST(HandleCompletionCallbacksTest, ArmedAcceptOverflowClosesExcessDuringDrive)
{
    bool overflowObserved = false;
    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);
    ASSERT_TRUE(cooperator.SubmitSync(
        [&](coop::Context* driver)
        {
            auto* ring = driver->GetCooperator()->GetUring();
            Listener listener;
            SocketPair targetSockets;
            coop::Signal release(driver);
            bool acceptReady = false;
            bool acceptDone = false;
            int consumerRuns = 0;
            uint64_t consumeCount = 0;
            coop::io::ArmedAccept* armed = nullptr;

            bool spawned = driver->GetCooperator()->Spawn(
                [&](coop::Context* consumer)
                {
                    {
                        coop::io::Descriptor descriptor(coop::io::borrowed, listener.fd, ring);
                        coop::Coordinator coordinator;
                        coop::io::ArmedAccept handle(consumer, descriptor, &coordinator, 1);
                        armed = &handle;
                        handle.Arm();
                        acceptReady = true;
                        release.Wait(consumer);
                        ++consumerRuns;
                        for (uint64_t i = 0; i < consumeCount; ++i)
                        {
                            int fd = handle.Next();
                            EXPECT_GE(fd, 0);
                            if (fd >= 0)
                                ::close(fd);
                        }
                    }
                    armed = nullptr;
                    acceptDone = true;
                });
            if (!spawned)
            {
                ADD_FAILURE() << "accept context did not spawn";
                driver->GetCooperator()->Shutdown();
                return;
            }
            while (!acceptReady)
                driver->Yield(true);

            // All connections finish their loopback handshake before the drive. The
            // scheduler does not re-enter between this burst and WaitWithoutYield(), so
            // one multishot accept can queue past its bound before its cancel is submitted.
            constexpr size_t kConnections = 24;
            std::array<int, kConnections> clients;
            clients.fill(-1);
            bool connected = true;
            for (int& client : clients)
            {
                client = listener.Connect(false);
                connected &= client >= 0;
            }
            EXPECT_TRUE(connected) << "loopback backlog was not fully populated";

            coop::io::Descriptor targetDescriptor(coop::io::borrowed, targetSockets.fd[0], ring);
            coop::Coordinator targetCoordinator;
            coop::io::Handle target(driver, targetDescriptor, &targetCoordinator);
            char targetByte{};
            EXPECT_TRUE(coop::io::Recv(target, &targetByte, 1));
            bool producerOk = false;
            std::thread producer(
                [&]
                {
                    bool timerOk = WaitForTimer(std::chrono::milliseconds(30));
                    bool writeOk = ::write(targetSockets.fd[1], "!", 1) == 1;
                    ::shutdown(targetSockets.fd[1], SHUT_WR);
                    producerOk = timerOk && writeOk;
                });

            {
                coop::test::CompletionObserver completions;
                EXPECT_EQ(target.WaitWithoutYield(), 1);
                EXPECT_EQ(targetByte, '!');
                EXPECT_EQ(consumerRuns, 0) << "NO_YIELD: accept consumer ran inside drive";
                if (armed)
                {
                    const auto data = reinterpret_cast<uintptr_t>(armed) | uintptr_t(0x6);
                    size_t accepted = 0;
                    for (size_t i = 0; i < completions.size; ++i)
                        accepted += completions.records[i].data == data &&
                                    completions.records[i].result >= 0;
                    EXPECT_EQ(accepted, armed->Delivered() + armed->Shed());
                    overflowObserved = armed->Shed() > 0;
                    EXPECT_GE(completions.Count(data | uintptr_t(0x1), IORING_CQE_F_MORE, 0), 1u)
                        << "accept cancel acknowledgment was not observed during drive";
                    consumeCount = armed->Delivered();
                }
                EXPECT_FALSE(completions.overflow);
            }

            // A shed connection is closed while the consumer is still parked. The
            // remaining queued accepted descriptors are retired by the consumer below.
            std::array<pollfd, kConnections> polls{};
            for (size_t i = 0; i < clients.size(); ++i)
            {
                polls[i].fd = clients[i];
                polls[i].events = POLLIN | POLLHUP | POLLERR;
            }
            if (overflowObserved)
                EXPECT_GT(::poll(polls.data(), polls.size(), 1000), 0);
            size_t closed = 0;
            for (int client : clients)
            {
                if (client < 0)
                    continue;
                char byte{};
                int n = ::recv(client, &byte, 1, MSG_DONTWAIT | MSG_PEEK);
                closed += n == 0 || (n < 0 && errno == ECONNRESET);
            }
            if (overflowObserved)
                EXPECT_GT(closed, 0u) << "no shed peer observed closure before consumer resumed";
            EXPECT_EQ(consumerRuns, 0) << "NO_YIELD: accept consumer ran before release";

            producer.join();
            EXPECT_TRUE(producerOk);
            release.Notify(driver, false);
            while (!acceptDone)
                driver->Yield(true);
            EXPECT_EQ(consumerRuns, 1);
            for (int client : clients)
                if (client >= 0)
                    ::close(client);
            driver->GetCooperator()->Shutdown();
        }));
    if (!overflowObserved)
        GTEST_SKIP() << "kernel cancellation preceded overflow (branch coverage gap)";
}

TEST(HandleCompletionCallbacksTest, UserspaceTimerWakeStaysDeferredThroughDrive)
{
    coop::CooperatorConfiguration config;
    config.timerMode = coop::TimerMode::UserspaceQueue;
    coop::Cooperator cooperator(config);
    coop::Thread thread(&cooperator);
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::mutex mutex;
    std::condition_variable changed;

    auto sleep = [&](std::chrono::milliseconds duration)
    {
        return cooperator.Submit(
            [&, duration](coop::Context* ctx)
            {
                {
                    std::lock_guard lock(mutex);
                    started.fetch_add(1, std::memory_order_release);
                }
                changed.notify_one();
                EXPECT_EQ(coop::time::Sleep(ctx, duration), coop::time::SleepResult::Ok);
                {
                    std::lock_guard lock(mutex);
                    finished.fetch_add(1, std::memory_order_release);
                }
                changed.notify_one();
            });
    };
    auto waitFor = [&](std::atomic<int>& count, int wanted)
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return count.load(std::memory_order_acquire) >= wanted; });
    };

    // The first idle period arms the queue's kernel timer. The earlier second
    // deadline causes the ordinary update SQE on the next idle period.
    //
    if (!sleep(std::chrono::milliseconds(200)))
    {
        ADD_FAILURE() << "first sleeper did not submit";
        cooperator.Shutdown();
        return;
    }
    waitFor(started, 1);
    EXPECT_TRUE(WaitForTimer(std::chrono::milliseconds(10)));
    if (!sleep(std::chrono::milliseconds(50)))
    {
        ADD_FAILURE() << "second sleeper did not submit";
        cooperator.Shutdown();
        return;
    }
    waitFor(started, 2);
    EXPECT_TRUE(WaitForTimer(std::chrono::milliseconds(10)));

    bool submitted = cooperator.SubmitSync(
        [&](coop::Context* driver)
        {
            auto* ring = driver->GetCooperator()->GetUring();
            SocketPair targetSockets;
            coop::io::Descriptor descriptor(coop::io::borrowed, targetSockets.fd[0], ring);
            coop::Coordinator coordinator;
            coop::io::Handle target(driver, descriptor, &coordinator);
            char byte{};
            EXPECT_TRUE(coop::io::Recv(target, &byte, 1));
            int finishedBefore = finished.load(std::memory_order_acquire);
            EXPECT_LT(finishedBefore, 2) << "timer observation window was missed";
            bool producerOk = false;
            std::thread producer(
                [&]
                {
                    bool timerOk = WaitForTimer(std::chrono::milliseconds(250));
                    bool writeOk = ::write(targetSockets.fd[1], "!", 1) == 1;
                    ::shutdown(targetSockets.fd[1], SHUT_WR);
                    producerOk = timerOk && writeOk;
                });
            // The update acknowledgment may have been reaped while the second
            // sleeper was starting. This scope establishes drive-time expiry only.
            {
                coop::test::CompletionObserver completions;
                EXPECT_EQ(target.WaitWithoutYield(), 1);
                EXPECT_EQ(byte, '!');
                bool sawExpiry = false;
                for (size_t i = 0; i < completions.size; ++i)
                {
                    const auto& cqe = completions.records[i];
                    sawExpiry |= cqe.data == coop::detail::kTimerTag && cqe.result == -ETIME &&
                                 !(cqe.flags & IORING_CQE_F_MORE);
                }
                EXPECT_TRUE(sawExpiry)
                    << "userspace scheduler timer expiry was not observed during drive";
                EXPECT_FALSE(completions.overflow);
                EXPECT_EQ(finished.load(std::memory_order_acquire), finishedBefore)
                    << "NO_YIELD: timer sleeper ran inside drive";
            }
            producer.join();
            EXPECT_TRUE(producerOk);
        });
    if (!submitted)
    {
        ADD_FAILURE() << "timer driver did not submit";
        cooperator.Shutdown();
        return;
    }

    waitFor(finished, 2);
    EXPECT_EQ(finished.load(std::memory_order_acquire), 2);
    cooperator.Shutdown();
}

} // namespace
