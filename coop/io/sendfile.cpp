#include "sendfile.h"

#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <linux/tls.h>
#include <spdlog/spdlog.h>

#include "descriptor.h"
#include "poll.h"
#include "read.h"
#include "send.h"

#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/io/handle.h"
#include "coop/io/pipe_pool.h"
#include "coop/io/detail/handle_extension.h"
#include "coop/self.h"
#include "coop/io/detail/yield_budget.h"
#include "coop/io/detail/send_progress.h"

namespace coop
{

namespace io
{

static int WaitForSendfileWritable(Descriptor& desc, bool killAware)
{
    if (killAware)
    {
        return PollKill(desc, POLLOUT);
    }
    return Poll(desc, POLLOUT);
}

static int SendfileImpl(Descriptor& desc, int in_fd, off_t offset, size_t count, bool killAware)
{
    SPDLOG_TRACE("sendfile fd={} in_fd={} offset={} count={}", desc.m_fd, in_fd, offset, count);
    for (;;)
    {
        ssize_t ret = ::sendfile(desc.m_fd, in_fd, &offset, count);
        if (ret > 0)
        {
            SPDLOG_TRACE("sendfile fd={} sent={}", desc.m_fd, ret);
            return (int)ret;
        }
        if (ret == 0) return 0;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            SPDLOG_TRACE("sendfile fd={} EAGAIN", desc.m_fd);
            int r = WaitForSendfileWritable(desc, killAware);
            if (r == -ECANCELED) return -ECANCELED;
            if (r < 0) return -1;
            continue;
        }

        spdlog::warn("sendfile fd={} errno={}", desc.m_fd, errno);
        return -1;
    }
}

int Sendfile(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    return SendfileImpl(desc, in_fd, offset, count, false);
}

int SendfileKill(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    return SendfileImpl(desc, in_fd, offset, count, true);
}

// splice has no MSG_NOSIGNAL flag. Keep its SIGPIPE local to this immediate syscall;
// the original thread mask is restored before the caller can yield or wait.
//
static ssize_t SpliceWithoutSigpipe(int in_fd, int out_fd, size_t count, bool ktls)
{
    sigset_t signals, saved, pending;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    int error = pthread_sigmask(SIG_BLOCK, &signals, &saved);
    if (error) { errno = error; return -1; }
    if (sigpending(&pending) < 0)
    {
        error = errno;
        pthread_sigmask(SIG_SETMASK, &saved, nullptr);
        errno = error;
        return -1;
    }
    const bool consume = sigismember(&pending, SIGPIPE) != 1;
    ssize_t result = ::splice(in_fd, nullptr, out_fd, nullptr, count,
        SPLICE_F_MOVE | SPLICE_F_NONBLOCK | (ktls ? SPLICE_F_MORE : 0));
    error = errno;
    if (consume)
    {
        timespec immediate{};
        int signal;
        do { signal = sigtimedwait(&signals, nullptr, &immediate); }
        while (signal < 0 && errno == EINTR);
        if (signal == SIGPIPE)
        {
            // kTLS can accept pages and raise SIGPIPE while flushing their record.
            // That record was not delivered, even if splice reports a positive count.
            result = -1;
            error = EPIPE;
        }
    }
    pthread_sigmask(SIG_SETMASK, &saved, nullptr);
    errno = error;
    return result;
}

// Flush a short kTLS record explicitly so both this flush and any kernel retry use
// MSG_NOSIGNAL. splice_eof flushes with flags zero, including during socket teardown.
//
static int FlushKtlsRecord(Descriptor& desc, detail::SendProgress& progress, bool finish = false)
{
    alignas(cmsghdr) char control[CMSG_SPACE(1)]{};
    msghdr message{};
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_TLS;
    cmsg->cmsg_type = TLS_SET_RECORD_TYPE;
    cmsg->cmsg_len = CMSG_LEN(1);
    *CMSG_DATA(cmsg) = 23; // TLS application data.
    for (;;)
    {
        time::Interval remaining;
        // The first post-append attempt stamps safe retry flags even if cancellation
        // arrived after splice. It never waits before observing the current budget.
        if (!finish)
            if (int error = progress.Prepare(remaining); error < 0) return error;
        finish = false;
        if (::sendmsg(desc.m_fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) return 0;
        if (errno == EINTR) { finish = true; continue; }
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -errno;
        if (int error = progress.Prepare(remaining); error < 0) return error;
        int result = remaining.count() > 0
            ? PollKill(desc, POLLOUT, remaining) : PollKill(desc, POLLOUT);
        if (result < 0) return result;
    }
}

// Force source-page acquisition onto an io_uring worker. Only the pipe's resident
// pages visit the nonblocking socket splice, so cold files never stall the scheduler.
//
int SendfileKill(Descriptor& desc, int in_fd, off_t offset, size_t count,
                 time::Interval timeout)
{
    detail::SendProgress progress(Self(), timeout);
    time::Interval remaining;
    if (int error = progress.Prepare(remaining); error < 0) return error;
    if (count == 0) return 0;
    tls_crypto_info info{};
    socklen_t length = sizeof(info);
    const bool ktls = getsockopt(desc.m_fd, SOL_TLS, TLS_TX, &info, &length) == 0;
    if (ktls)
    {
        if (int error = FlushKtlsRecord(desc, progress); error < 0) return error;
        if (int error = progress.Prepare(remaining); error < 0) return error;
    }
    // Keep kTLS below its full-record boundary until FlushKtlsRecord supplies NOSIGNAL.
    const size_t limit = detail::SendProgress::MAX_WRITE - (ktls ? 1 : 0);
    PipeLease pipe(desc.m_ring->GetPipePool());
    if (!pipe) return -errno;
    int n;
    {
        Coordinator coord;
        Handle handle(Self(), desc.m_ring, &coord);
        auto* sqe = detail::HandleExtension::GetSqe(handle);
        io_uring_prep_splice(sqe, in_fd, offset, pipe.Fds()[1], -1,
            std::min(count, limit), SPLICE_F_MOVE);
        sqe->flags |= IOSQE_ASYNC;
        if (remaining.count() > 0) handle.SubmitWithTimeout(sqe, remaining);
        else handle.Submit(sqe);
        n = handle.WaitKill();
        if (n < 0)
        {
            // A canceled operation may have filled the pipe before its CQE drained.
            pipe.MarkDirty();
            return handle.TimedOut() ? -ETIMEDOUT : n;
        }
    }
    if (n <= 0) return n;
    for (;;)
    {
        if (int error = progress.Prepare(remaining); error < 0)
        {
            pipe.MarkDirty();
            return error;
        }
        ssize_t sent = SpliceWithoutSigpipe(pipe.Fds()[0], desc.m_fd, static_cast<size_t>(n), ktls);
        if (sent > 0)
        {
            if (sent < n) pipe.MarkDirty();
            if (ktls)
            {
                if (int error = FlushKtlsRecord(desc, progress, true); error < 0)
                {
                    pipe.MarkDirty();
                    return error;
                }
            }
            progress.Progress(static_cast<size_t>(sent));
            return static_cast<int>(sent);
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            int result = remaining.count() > 0
                ? PollKill(desc, POLLOUT, remaining) : PollKill(desc, POLLOUT);
            if (result >= 0) continue;
            pipe.MarkDirty();
            return result;
        }
        pipe.MarkDirty();
        return sent == 0 ? -EIO : -errno;
    }
}

static int SendfileAllImpl(Descriptor& desc, int in_fd, off_t offset, size_t count, bool killAware)
{
    size_t total = 0;
    detail::YieldBudget budget(Self());
    while (total < count)
    {
        int sent = killAware
            ? SendfileKill(desc, in_fd, offset + (off_t)total, count - total)
            : Sendfile(desc, in_fd, offset + (off_t)total, count - total);
        if (sent <= 0) return sent;
        total += sent;
        if (total < count)
        {
            budget.Charge(sent);
        }
    }
    return (int)count;
}

int SendfileAll(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    return SendfileAllImpl(desc, in_fd, offset, count, false);
}

int SendfileAllKill(Descriptor& desc, int in_fd, off_t offset, size_t count)
{
    return SendfileAllImpl(desc, in_fd, offset, count, true);
}

} // end namespace coop::io
} // end namespace coop
