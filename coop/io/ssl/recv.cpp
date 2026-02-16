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

// kTLS RX recv — read() directly, kernel handles decryption. Falls back to io::Poll when
// no data available (EAGAIN). First call typically returns EAGAIN (receiver called before
// sender), but if data is already buffered, returns immediately without uring.
//
static int RecvKtls(Connection& conn, void* buf, size_t size)
{
    spdlog::trace("ssl ktls recv fd={} maxsize={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        ssize_t ret = ::read(conn.m_desc.m_fd, buf, size);
        if (ret > 0)
        {
            spdlog::trace("ssl ktls recv fd={} read={}", conn.m_desc.m_fd, ret);
            return (int)ret;
        }
        if (ret == 0) return 0;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            spdlog::trace("ssl ktls recv fd={} EAGAIN", conn.m_desc.m_fd);
            int r = io::Poll(conn.m_desc, POLLIN);
            if (r < 0) return -1;
            continue;
        }

        spdlog::warn("ssl ktls recv fd={} errno={}", conn.m_desc.m_fd, errno);
        return -1;
    }
}

// Socket BIO recv — SSL_read operates on the real fd, io::Poll for cooperative waiting.
// Used when kTLS didn't activate but the connection uses a socket BIO.
//
static int RecvSocketBio(Connection& conn, void* buf, size_t size)
{
    spdlog::trace("ssl socket-bio recv fd={} maxsize={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        int ret = SSL_read(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            spdlog::trace("ssl socket-bio recv fd={} read={}", conn.m_desc.m_fd, ret);
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_READ:
        {
            spdlog::trace("ssl socket-bio recv fd={} WANT_READ", conn.m_desc.m_fd);
            int r = io::Poll(conn.m_desc, POLLIN);
            if (r < 0) return -1;
            break;
        }

        case SSL_ERROR_WANT_WRITE:
        {
            // Renegotiation
            //
            spdlog::trace("ssl socket-bio recv fd={} WANT_WRITE", conn.m_desc.m_fd);
            int r = io::Poll(conn.m_desc, POLLOUT);
            if (r < 0) return -1;
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
//   kTLS RX:     read() directly, kernel decrypts + io::Poll on EAGAIN
//   Socket BIO:  SSL_read on real fd + io::Poll for cooperative waiting
//   Memory BIO:  FeedRead -> rbio -> SSL_read (existing path)
//
// Returns bytes read on success, negative on error, 0 on clean shutdown.
//
int Recv(Connection& conn, void* buf, size_t size)
{
    // kTLS RX: kernel handles decryption, read() directly
    //
    if (conn.m_ktlsRx)
    {
        return RecvKtls(conn, buf, size);
    }

    // Socket BIO without kTLS: SSL_read on real fd + io::Poll
    //
    if (conn.m_buffer == nullptr)
    {
        return RecvSocketBio(conn, buf, size);
    }

    // Memory BIO: existing path
    //
    spdlog::trace("ssl recv fd={} maxsize={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        int ret = SSL_read(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            spdlog::trace("ssl recv fd={} read={}", conn.m_desc.m_fd, ret);
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_READ:
            spdlog::trace("ssl recv fd={} WANT_READ", conn.m_desc.m_fd);
            if (conn.FlushWrite() < 0)
            {
                return -1;
            }
            if (conn.FeedRead() <= 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_WANT_WRITE:
            // Can happen during TLS renegotiation.
            //
            spdlog::trace("ssl recv fd={} WANT_WRITE", conn.m_desc.m_fd);
            if (conn.FlushWrite() < 0)
            {
                return -1;
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

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
