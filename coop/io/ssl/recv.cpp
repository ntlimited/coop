#include "recv.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

#include "connection.h"
#include "coop/io/descriptor.h"
#include "coop/io/poll.h"

namespace coop
{

namespace io
{

namespace ssl
{

// Cooperative wait for socket readiness. A positive timeout bounds the wait and returns
// -ETIMEDOUT; zero waits until readiness, kill, or error. Negative results (timeout, kill,
// hard error) propagate so HTTP keep-alive can exit instead of hanging.
//
static int WaitSocket(Descriptor& desc, unsigned mask, bool killAware, time::Interval timeout)
{
    if (timeout.count() > 0)
    {
        return killAware
            ? io::PollKill(desc, mask, timeout)
            : io::Poll(desc, mask, timeout);
    }
    return killAware ? io::PollKill(desc, mask) : io::Poll(desc, mask);
}

// kTLS RX recv — read() directly, kernel handles decryption. Falls back to readiness waits when
// no data available (EAGAIN). First call typically returns EAGAIN (receiver called before
// sender), but if data is already buffered, returns immediately without uring.
//
static int RecvKtls(Connection& conn, void* buf, size_t size, bool killAware,
                    time::Interval timeout)
{
    SPDLOG_TRACE("ssl ktls recv fd={} maxsize={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        ssize_t ret = ::read(conn.m_desc.m_fd, buf, size);
        if (ret > 0)
        {
            SPDLOG_TRACE("ssl ktls recv fd={} read={}", conn.m_desc.m_fd, ret);
            return (int)ret;
        }
        if (ret == 0) return 0;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            SPDLOG_TRACE("ssl ktls recv fd={} EAGAIN", conn.m_desc.m_fd);
            int r = WaitSocket(conn.m_desc, POLLIN, killAware, timeout);
            if (r < 0) return r;
            continue;
        }

        spdlog::warn("ssl ktls recv fd={} errno={}", conn.m_desc.m_fd, errno);
        return -1;
    }
}

// Socket BIO recv — SSL_read operates on the real fd, readiness waits for cooperative waiting.
// Used when kTLS didn't activate but the connection uses a socket BIO.
//
static int RecvSocketBio(Connection& conn, void* buf, size_t size, bool killAware,
                         time::Interval timeout)
{
    SPDLOG_TRACE("ssl socket-bio recv fd={} maxsize={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        int ret = SSL_read(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            SPDLOG_TRACE("ssl socket-bio recv fd={} read={}", conn.m_desc.m_fd, ret);
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_READ:
        {
            SPDLOG_TRACE("ssl socket-bio recv fd={} WANT_READ", conn.m_desc.m_fd);
            int r = WaitSocket(conn.m_desc, POLLIN, killAware, timeout);
            if (r < 0) return r;
            break;
        }

        case SSL_ERROR_WANT_WRITE:
        {
            // Renegotiation
            //
            SPDLOG_TRACE("ssl socket-bio recv fd={} WANT_WRITE", conn.m_desc.m_fd);
            int r = WaitSocket(conn.m_desc, POLLOUT, killAware, timeout);
            if (r < 0) return r;
            break;
        }

        case SSL_ERROR_ZERO_RETURN:
            return 0;

        default:
            spdlog::warn("ssl socket-bio recv fd={} error={}", conn.m_desc.m_fd, err);
            return -1;
        }
    }
}

// Receive plaintext data from a TLS connection. Dispatches based on connection mode:
//
//   kTLS RX:     read() directly, kernel decrypts + readiness waits on EAGAIN
//   Socket BIO:  SSL_read on real fd + readiness waits
//   Memory BIO:  FeedRead -> rbio -> SSL_read (existing path)
//
// A positive timeout bounds each nested socket wait in this Recv (keep-alive idle is one
// wait). Zero means wait until data, kill, or error. Returns bytes read on success,
// negative on error (including -ETIMEDOUT / -ECANCELED), 0 on clean shutdown.
//
int RecvImpl(Connection& conn, void* buf, size_t size, bool killAware, time::Interval timeout)
{
    // kTLS RX: kernel handles decryption, read() directly
    //
    if (conn.m_ktlsRx)
    {
        return RecvKtls(conn, buf, size, killAware, timeout);
    }

    // Socket BIO without kTLS: SSL_read on real fd + readiness waits
    //
    if (conn.m_buffer == nullptr)
    {
        return RecvSocketBio(conn, buf, size, killAware, timeout);
    }

    // Memory BIO: existing path
    //
    SPDLOG_TRACE("ssl recv fd={} maxsize={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        int ret = SSL_read(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            SPDLOG_TRACE("ssl recv fd={} read={}", conn.m_desc.m_fd, ret);
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_READ:
            SPDLOG_TRACE("ssl recv fd={} WANT_READ", conn.m_desc.m_fd);
            if (int w = conn.FlushWrite(killAware, timeout); w < 0)
            {
                return w;
            }
            if (int n = conn.FeedRead(killAware, timeout); n <= 0)
            {
                return n < 0 ? n : -1;
            }
            break;

        case SSL_ERROR_WANT_WRITE:
            // Can happen during TLS renegotiation.
            //
            SPDLOG_TRACE("ssl recv fd={} WANT_WRITE", conn.m_desc.m_fd);
            if (int w = conn.FlushWrite(killAware, timeout); w < 0)
            {
                return w;
            }
            break;

        case SSL_ERROR_ZERO_RETURN:
            return 0;

        default:
            spdlog::warn("ssl recv fd={} error={}", conn.m_desc.m_fd, err);
            return -1;
        }
    }
}

int Recv(Connection& conn, void* buf, size_t size)
{
    return RecvImpl(conn, buf, size, false, {});
}

int Recv(Connection& conn, void* buf, size_t size, time::Interval timeout)
{
    return RecvImpl(conn, buf, size, false, timeout);
}

int RecvKill(Connection& conn, void* buf, size_t size)
{
    return RecvImpl(conn, buf, size, true, {});
}

int RecvKill(Connection& conn, void* buf, size_t size, time::Interval timeout)
{
    return RecvImpl(conn, buf, size, true, timeout);
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
