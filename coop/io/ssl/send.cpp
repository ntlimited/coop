#include "send.h"

#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

#include "connection.h"
#include "coop/io/detail/send_progress.h"
#include "coop/io/descriptor.h"
#include "coop/io/poll.h"
#include "coop/self.h"

namespace coop
{
namespace io
{
namespace ssl
{

int TrySend(Connection& conn, const void* buf, size_t size)
{
    if (conn.m_writeFailed || conn.m_finalSendAttempted) return -EPIPE;
    conn.m_finalSendAttempted = true;
    size = std::min(size, io::detail::SendProgress::MAX_WRITE);
    auto fail = [&](int error) { conn.m_writeFailed = true; return error; };
    if (conn.m_ktlsTx)
    {
        ssize_t n = ::send(conn.m_desc.m_fd, buf, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) return fail(-errno);
        if (static_cast<size_t>(n) != size) conn.m_writeFailed = true;
        return static_cast<int>(n);
    }
    errno = 0;
    int n = SSL_write(conn.m_ssl, buf, static_cast<int>(size));
    int savedErrno = errno;
    int error = n > 0 ? SSL_ERROR_NONE : SSL_get_error(conn.m_ssl, n);
    if (n <= 0)
    {
        if (error == SSL_ERROR_SYSCALL && savedErrno) return fail(-savedErrno);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return fail(-EAGAIN);
        return fail(error == SSL_ERROR_ZERO_RETURN ? -EPIPE : -EIO);
    }
    if (conn.m_buffer)
    {
        size_t pending = BIO_ctrl_pending(conn.m_wbio);
        if (pending > conn.m_bufferSize) return fail(-EAGAIN);
        int bytes = BIO_read(conn.m_wbio, conn.m_buffer, pending);
        if (bytes <= 0) return fail(-EIO);
        ssize_t sent = ::send(conn.m_desc.m_fd, conn.m_buffer, bytes,
                              MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent != bytes) return fail(sent < 0 ? -errno : -EAGAIN);
    }
    return n;
}

static int WaitSocket(Connection& conn, unsigned mask, bool killAware,
                      io::detail::SendProgress& progress)
{
    time::Interval timeout;
    if (int error = progress.Prepare(timeout); error < 0) return error;
    if (timeout.count() > 0)
        return killAware ? io::PollKill(conn.m_desc, mask, timeout)
                         : io::Poll(conn.m_desc, mask, timeout);
    return killAware ? io::PollKill(conn.m_desc, mask) : io::Poll(conn.m_desc, mask);
}

// Each send accepts at most one TLS record's plaintext. A successful return also
// means its encrypted output reached the socket; encryption alone is not progress.
//
int SendImpl(Connection& conn, const void* buf, size_t size, bool killAware,
             time::Interval timeout, io::detail::SendProgress* suppliedProgress)
{
    if (conn.m_writeFailed || conn.m_finalSendAttempted) return -EPIPE;
    auto fail = [&](int error) { conn.m_writeFailed = true; return error; };
    io::detail::SendProgress localProgress(killAware ? Self() : nullptr, timeout);
    auto& progress = suppliedProgress ? *suppliedProgress : localProgress;
    SPDLOG_TRACE("ssl send fd={} size={}", conn.m_desc.m_fd, size);
    size = std::min(size, io::detail::SendProgress::MAX_WRITE);
    if (size == 0) return 0;
    for (;;)
    {
        time::Interval remaining;
        if (int error = progress.Prepare(remaining); error < 0)
        {
            if (error == -ECANCELED) return TrySend(conn, buf, size);
            return fail(error);
        }
        if (conn.m_ktlsTx)
        {
            ssize_t n = ::send(conn.m_desc.m_fd, buf, size, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n >= 0) return static_cast<int>(n);
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return fail(-errno);
            if (int error = WaitSocket(conn, POLLOUT, killAware, progress); error < 0)
                return fail(error);
            continue;
        }

        BIO* output = SSL_get_wbio(conn.m_ssl);
        uint64_t before = conn.m_buffer ? 0 : BIO_number_written(output);
        errno = 0;
        int n = SSL_write(conn.m_ssl, buf, static_cast<int>(size));
        int savedErrno = errno;
        int error = n > 0 ? SSL_ERROR_NONE : SSL_get_error(conn.m_ssl, n);
        if (!conn.m_buffer)
        {
            uint64_t written = BIO_number_written(output) - before;
            if (written) progress.Progress(static_cast<size_t>(written));
        }
        if (n > 0)
        {
            if (conn.m_buffer)
            {
                int error = conn.FlushWrite(killAware, remaining, &progress);
                if (error < 0) return fail(error);
            }
            SPDLOG_TRACE("ssl send fd={} written={}", conn.m_desc.m_fd, n);
            return n;
        }
        if (error == SSL_ERROR_ZERO_RETURN) { conn.m_writeFailed = true; return 0; }
        if (error != SSL_ERROR_WANT_WRITE && error != SSL_ERROR_WANT_READ)
        {
            spdlog::warn("ssl send fd={} error={}", conn.m_desc.m_fd, error);
            return fail(error == SSL_ERROR_SYSCALL && savedErrno ? -savedErrno : -EIO);
        }
        SPDLOG_TRACE("ssl send fd={} want={}", conn.m_desc.m_fd, error);
        if (conn.m_buffer)
        {
            if (int result = conn.FlushWrite(killAware, remaining, &progress); result < 0)
                return fail(result);
            if (error == SSL_ERROR_WANT_READ)
            {
                if (int result = progress.Prepare(remaining); result < 0) return fail(result);
                int result = conn.FeedRead(killAware, remaining);
                if (result <= 0) return fail(result < 0 ? result : -ECONNRESET);
            }
        }
        else
        {
            int result = WaitSocket(conn, error == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN,
                                    killAware, progress);
            if (result < 0) return fail(result);
        }
    }
}

int Send(Connection& conn, const void* buf, size_t size)
{
    return SendImpl(conn, buf, size, false, {}, nullptr);
}

int SendKill(Connection& conn, const void* buf, size_t size)
{
    return SendImpl(conn, buf, size, true, {}, nullptr);
}

int SendKill(Connection& conn, const void* buf, size_t size, time::Interval timeout)
{
    return SendImpl(conn, buf, size, true, timeout, nullptr);
}

static int SendAllImpl(Connection& conn, const void* buf, size_t size, bool killAware)
{
    size_t offset = 0;
    while (offset < size)
    {
        int sent = SendImpl(conn, static_cast<const char*>(buf) + offset, size - offset,
                            killAware, {}, nullptr);
        if (sent <= 0) return sent;
        offset += sent;
    }
    return static_cast<int>(size);
}

int SendAll(Connection& conn, const void* buf, size_t size)
{
    return SendAllImpl(conn, buf, size, false);
}

int SendAllKill(Connection& conn, const void* buf, size_t size)
{
    return SendAllImpl(conn, buf, size, true);
}

} // namespace coop::io::ssl
} // namespace coop::io
} // namespace coop
