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

// kTLS TX send — write() directly, kernel handles encryption. Falls back to io::Poll when
// the socket buffer is full (EAGAIN). Avoids uring for the common case where the write
// succeeds immediately, matching the synchronous-when-possible pattern of SendSocketBio.
//
static int SendKtls(Connection& conn, const void* buf, size_t size)
{
    spdlog::trace("ssl ktls send fd={} size={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        ssize_t ret = ::write(conn.m_desc.m_fd, buf, size);
        if (ret > 0)
        {
            spdlog::trace("ssl ktls send fd={} written={}", conn.m_desc.m_fd, ret);
            return (int)ret;
        }
        if (ret == 0) return 0;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            spdlog::trace("ssl ktls send fd={} EAGAIN", conn.m_desc.m_fd);
            int r = io::Poll(conn.m_desc, POLLOUT);
            if (r < 0) return -1;
            continue;
        }

        spdlog::warn("ssl ktls send fd={} errno={}", conn.m_desc.m_fd, errno);
        return -1;
    }
}

// Socket BIO send — SSL_write operates on the real fd, io::Poll for cooperative waiting.
// Used when kTLS didn't activate but the connection uses a socket BIO.
//
static int SendSocketBio(Connection& conn, const void* buf, size_t size)
{
    spdlog::trace("ssl socket-bio send fd={} size={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        int ret = SSL_write(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            spdlog::trace("ssl socket-bio send fd={} written={}", conn.m_desc.m_fd, ret);
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_WRITE:
        {
            spdlog::trace("ssl socket-bio send fd={} WANT_WRITE", conn.m_desc.m_fd);
            int r = io::Poll(conn.m_desc, POLLOUT);
            if (r < 0) return -1;
            break;
        }

        case SSL_ERROR_WANT_READ:
        {
            // Renegotiation
            //
            spdlog::trace("ssl socket-bio send fd={} WANT_READ", conn.m_desc.m_fd);
            int r = io::Poll(conn.m_desc, POLLIN);
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
//   kTLS TX:     write() directly, kernel encrypts + io::Poll on EAGAIN
//   Socket BIO:  SSL_write on real fd + io::Poll for cooperative waiting
//   Memory BIO:  SSL_write -> wbio -> FlushWrite -> io::Send (existing path)
//
// Returns bytes written on success, negative on error, 0 on clean shutdown.
//
int Send(Connection& conn, const void* buf, size_t size)
{
    // kTLS TX: kernel handles encryption, write() directly
    //
    if (conn.m_ktlsTx)
    {
        return SendKtls(conn, buf, size);
    }

    // Socket BIO without kTLS: SSL_write on real fd + io::Poll
    //
    if (conn.m_buffer == nullptr)
    {
        return SendSocketBio(conn, buf, size);
    }

    // Memory BIO: existing path
    //
    spdlog::trace("ssl send fd={} size={}", conn.m_desc.m_fd, size);
    for (;;)
    {
        int ret = SSL_write(conn.m_ssl, buf, size);
        if (ret > 0)
        {
            // Plaintext was encrypted. Push the ciphertext out.
            //
            spdlog::trace("ssl send fd={} written={}", conn.m_desc.m_fd, ret);
            if (conn.FlushWrite() < 0)
            {
                return -1;
            }
            return ret;
        }

        int err = SSL_get_error(conn.m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_WRITE:
            spdlog::trace("ssl send fd={} WANT_WRITE", conn.m_desc.m_fd);
            if (conn.FlushWrite() < 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_WANT_READ:
            // Can happen during TLS renegotiation.
            //
            spdlog::trace("ssl send fd={} WANT_READ", conn.m_desc.m_fd);
            if (conn.FlushWrite() < 0)
            {
                return -1;
            }
            if (conn.FeedRead() <= 0)
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

int SendAll(Connection& conn, const void* buf, size_t size)
{
    size_t offset = 0;
    while (offset < size)
    {
        int sent = Send(conn, (const char*)buf + offset, size - offset);
        if (sent <= 0)
        {
            return sent;
        }
        offset += sent;
    }
    return (int)size;
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
