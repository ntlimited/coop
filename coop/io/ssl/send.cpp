#include "send.h"

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

// kTLS TX send — write() directly, kernel handles encryption. Falls back to readiness waits when
// the socket buffer is full (EAGAIN). Avoids uring for the common case where the write
// succeeds immediately, matching the synchronous-when-possible pattern of SendSocketBio.
//
static int SendKtls(Connection& conn, const void* buf, size_t size, bool killAware)
{
    SPDLOG_TRACE("ssl ktls send fd={} size={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        ssize_t ret = ::write(conn.m_desc.m_fd, buf, size);
        if (ret > 0)
        {
            SPDLOG_TRACE("ssl ktls send fd={} written={}", conn.m_desc.m_fd, ret);
            return (int)ret;
        }
        if (ret == 0) return 0;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            SPDLOG_TRACE("ssl ktls send fd={} EAGAIN", conn.m_desc.m_fd);
            int r = killAware ? io::PollKill(conn.m_desc, POLLOUT) : io::Poll(conn.m_desc, POLLOUT);
            if (r < 0) return -1;
            continue;
        }

        spdlog::warn("ssl ktls send fd={} errno={}", conn.m_desc.m_fd, errno);
        return -1;
    }
}

// Socket BIO send — SSL_write operates on the real fd, readiness waits for cooperative waiting.
// Used when kTLS didn't activate but the connection uses a socket BIO.
//
static int SendSocketBio(Connection& conn, const void* buf, size_t size, bool killAware)
{
    SPDLOG_TRACE("ssl socket-bio send fd={} size={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        int ret = SSL_write(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            SPDLOG_TRACE("ssl socket-bio send fd={} written={}", conn.m_desc.m_fd, ret);
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_WRITE:
        {
            SPDLOG_TRACE("ssl socket-bio send fd={} WANT_WRITE", conn.m_desc.m_fd);
            int r = killAware ? io::PollKill(conn.m_desc, POLLOUT) : io::Poll(conn.m_desc, POLLOUT);
            if (r < 0) return -1;
            break;
        }

        case SSL_ERROR_WANT_READ:
        {
            // Renegotiation
            //
            SPDLOG_TRACE("ssl socket-bio send fd={} WANT_READ", conn.m_desc.m_fd);
            int r = killAware ? io::PollKill(conn.m_desc, POLLIN) : io::Poll(conn.m_desc, POLLIN);
            if (r < 0) return -1;
            break;
        }

        case SSL_ERROR_ZERO_RETURN:
            return 0;

        default:
            spdlog::warn("ssl socket-bio send fd={} error={}", conn.m_desc.m_fd, err);
            return -1;
        }
    }
}

// Send plaintext data over a TLS connection. Dispatches based on connection mode:
//
//   kTLS TX:     write() directly, kernel encrypts + readiness waits on EAGAIN
//   Socket BIO:  SSL_write on real fd + readiness waits
//   Memory BIO:  SSL_write -> wbio -> FlushWrite -> io::Send (existing path)
//
// Returns bytes written on success, negative on error, 0 on clean shutdown.
//
int SendImpl(Connection& conn, const void* buf, size_t size, bool killAware)
{
    // kTLS TX: kernel handles encryption, write() directly
    //
    if (conn.m_ktlsTx)
    {
        return SendKtls(conn, buf, size, killAware);
    }

    // Socket BIO without kTLS: SSL_write on real fd + readiness waits
    //
    if (conn.m_buffer == nullptr)
    {
        return SendSocketBio(conn, buf, size, killAware);
    }

    // Memory BIO: existing path
    //
    SPDLOG_TRACE("ssl send fd={} size={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        int ret = SSL_write(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            // Plaintext was encrypted. Push the ciphertext out.
            //
            SPDLOG_TRACE("ssl send fd={} written={}", conn.m_desc.m_fd, ret);
            if (conn.FlushWrite(killAware) < 0)
            {
                return -1;
            }
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_WRITE:
            SPDLOG_TRACE("ssl send fd={} WANT_WRITE", conn.m_desc.m_fd);
            if (conn.FlushWrite(killAware) < 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_WANT_READ:
            // Can happen during TLS renegotiation.
            //
            SPDLOG_TRACE("ssl send fd={} WANT_READ", conn.m_desc.m_fd);
            if (conn.FlushWrite(killAware) < 0)
            {
                return -1;
            }
            if (conn.FeedRead(killAware) <= 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_ZERO_RETURN:
            return 0;

        default:
            spdlog::warn("ssl send fd={} error={}", conn.m_desc.m_fd, err);
            return -1;
        }
    }
}

int Send(Connection& conn, const void* buf, size_t size)
{
    return SendImpl(conn, buf, size, false);
}

int SendKill(Connection& conn, const void* buf, size_t size)
{
    return SendImpl(conn, buf, size, true);
}

static int SendAllImpl(Connection& conn, const void* buf, size_t size, bool killAware)
{
    size_t offset = 0;
    while (offset < size)
    {
        int sent = killAware
            ? SendKill(conn, (const char*)buf + offset, size - offset)
            : Send(conn, (const char*)buf + offset, size - offset);
        if (sent <= 0)
        {
            return sent;
        }
        offset += sent;
    }
    return (int)size;
}

int SendAll(Connection& conn, const void* buf, size_t size)
{
    return SendAllImpl(conn, buf, size, false);
}

int SendAllKill(Connection& conn, const void* buf, size_t size)
{
    return SendAllImpl(conn, buf, size, true);
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
