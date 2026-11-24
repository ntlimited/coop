#include "connection.h"
#include "coop/io/detail/send_progress.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <spdlog/spdlog.h>

#include "context.h"
#include "coop/io/descriptor.h"
#include "coop/io/poll.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/time/now.h"
#include "coop/context.h"

namespace coop
{

namespace io
{

namespace ssl
{

Connection::Connection(Context& ctx, Descriptor& desc, char* buffer, size_t bufferSize)
: m_desc(desc)
, m_buffer(buffer)
, m_bufferSize(bufferSize)
{
    m_ssl = SSL_new(ctx.m_ctx);
    assert(m_ssl);

    // Create memory BIOs. OpenSSL will read ciphertext from rbio and write ciphertext to wbio.
    // We shuttle data between these BIOs and the real socket using io::Send/io::Recv. This
    // decouples the TLS state machine from the socket entirely, letting io_uring handle all
    // actual I/O.
    //
    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    assert(m_rbio && m_wbio);

    // SSL_set_bio transfers ownership of both BIOs to the SSL object — SSL_free will free them.
    //
    SSL_set_bio(m_ssl, m_rbio, m_wbio);

    if (ctx.m_mode == Mode::Server)
    {
        SSL_set_accept_state(m_ssl);
    }
    else
    {
        SSL_set_connect_state(m_ssl);
    }
}

Connection::Connection(Context& ctx, Descriptor& desc, SocketBio)
: m_desc(desc)
, m_buffer(nullptr)
, m_bufferSize(0)
{
    m_ssl = SSL_new(ctx.m_ctx);
    assert(m_ssl);

    // Attach OpenSSL directly to the real socket fd. This lets OpenSSL install kTLS state
    // into the kernel after handshake. SSL_set_fd creates socket BIOs internally.
    //
    SSL_set_fd(m_ssl, desc.m_fd);
    m_rbio = nullptr;
    m_wbio = nullptr;

    // The socket must be non-blocking for cooperative handshake via readiness waits.
    //
    int flags = fcntl(desc.m_fd, F_GETFL, 0);
    fcntl(desc.m_fd, F_SETFL, flags | O_NONBLOCK);

    // Disable Nagle's algorithm. kTLS frames each write() as a complete TLS record. With Nagle
    // enabled, the TCP layer may delay sending the record, causing the receiver to stall waiting
    // for data sitting in the sender's buffer. This creates catastrophic O(n) per-byte overhead
    // on loopback — 500x slower than plaintext at 16KB messages.
    //
    int on = 1;
    setsockopt(desc.m_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    if (ctx.m_mode == Mode::Server)
    {
        SSL_set_accept_state(m_ssl);
    }
    else
    {
        SSL_set_connect_state(m_ssl);
    }
}

Connection::~Connection()
{
    SSL_free(m_ssl);
}

bool Connection::SetServerName(const char* dnsName)
{
    return dnsName && *dnsName && SSL_set_tlsext_host_name(m_ssl, dnsName) == 1;
}

bool Connection::SetVerifyHost(const char* dnsName)
{
    return dnsName && *dnsName && SSL_set1_host(m_ssl, dnsName) == 1;
}

bool Connection::SetVerifyIp(const char* address)
{
    return address && *address
        && X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(m_ssl), address) == 1;
}

// Drain any pending data from OpenSSL's write BIO and send it over the wire via io::Send. This
// must be called after every SSL operation that might produce output (handshake, SSL_write, etc).
//
// Returns 0 on success, negative on I/O error.
//
int Connection::FlushWrite(bool killAware, time::Interval timeout,
                           io::detail::SendProgress* suppliedProgress)
{
    if (m_writeFailed) return -EPIPE;
    auto fail = [&](int error) { m_writeFailed = true; return error; };
    io::detail::SendProgress localProgress(killAware ? Self() : nullptr, timeout);
    auto& progress = suppliedProgress ? *suppliedProgress : localProgress;
    while (BIO_ctrl_pending(m_wbio) > 0)
    {
        int n = BIO_read(m_wbio, m_buffer, m_bufferSize);
        if (n <= 0)
        {
            break;
        }
        SPDLOG_TRACE("ssl flush_write fd={} wbio_bytes={}", m_desc.m_fd, n);

        int at = 0;
        while (at < n)
        {
            time::Interval remaining;
            if (int error = progress.Prepare(remaining); error < 0) return fail(error);
            int sent;
            if (remaining.count() > 0)
            {
                sent = killAware
                    ? io::SendKill(m_desc, &m_buffer[at], n - at, 0, remaining)
                    : io::Send(m_desc, &m_buffer[at], n - at, 0, remaining);
            }
            else
            {
                sent = killAware
                    ? io::SendKill(m_desc, &m_buffer[at], n - at)
                    : io::Send(m_desc, &m_buffer[at], n - at);
            }
            if (sent <= 0)
            {
                spdlog::warn("ssl flush_write fd={} send failed={}", m_desc.m_fd, sent);
                return fail(sent < 0 ? sent : -EIO);
            }
            at += sent;
            progress.Progress(static_cast<size_t>(sent));
        }
    }
    return 0;
}

// Read ciphertext from the wire via io::Recv and feed it into OpenSSL's read BIO. Called when
// SSL needs more input data (SSL_ERROR_WANT_READ).
//
// Returns bytes fed on success, 0 on clean disconnect, negative on error.
//
int Connection::FeedRead(bool killAware, time::Interval timeout)
{
    int n;
    if (timeout.count() > 0)
    {
        n = killAware
            ? io::RecvKill(m_desc, m_buffer, m_bufferSize, 0, timeout)
            : io::Recv(m_desc, m_buffer, m_bufferSize, 0, timeout);
    }
    else
    {
        n = killAware
            ? io::RecvKill(m_desc, m_buffer, m_bufferSize)
            : io::Recv(m_desc, m_buffer, m_bufferSize);
    }
    if (n <= 0)
    {
        SPDLOG_TRACE("ssl feed_read fd={} recv={}", m_desc.m_fd, n);
        return n;
    }

    int written = BIO_write(m_rbio, m_buffer, n);
    assert(written == n);
    SPDLOG_TRACE("ssl feed_read fd={} recv={} rbio_fed={}", m_desc.m_fd, n, written);
    return written;
}

// Dispatch to the appropriate handshake implementation based on the BIO mode.
//
int Connection::Handshake()
{
    if (m_buffer == nullptr)
    {
        return HandshakeSocketBio(false);
    }
    return HandshakeMemoryBio(false);
}

int Connection::HandshakeKill()
{
    if (m_buffer == nullptr)
    {
        return HandshakeSocketBio(true);
    }
    return HandshakeMemoryBio(true);
}

int Connection::ShutdownWriteKill(time::Interval timeout)
{
    if (m_shutdownWriteFailed || m_writeFailed) return -EPIPE;
    auto* ctx = Self();
    // An orderly closure is a bounded final operation even when normal IO is unbounded.
    // The server passes its timeout, with a one-second cap for this alert alone.
    //
    if (timeout.count() <= 0) timeout = std::chrono::seconds(1);
    int64_t budget = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
    int64_t started = time::MonotonicMicros();
    auto remaining = [&]() -> time::Interval
    {
        int64_t now = time::MonotonicMicros();
        if (ctx && !ctx->IsKilled() && ctx->DeadlineUs() != 0 && now >= ctx->DeadlineUs())
            ctx->KillSelf(KillCause::Deadline);
        if (ctx && ctx->IsKilled()) return {};
        int64_t left = budget - (now - started);
        if (ctx && ctx->DeadlineUs() != 0)
            left = std::min(left, ctx->DeadlineUs() - now);
        return left > 0 ? std::chrono::microseconds(left) : time::Interval{};
    };
    auto fail = [&](int error)
    {
        m_shutdownWriteFailed = true;
        m_writeFailed = true;
        return error;
    };
    bool first = true;
    for (;;)
    {
        auto left = remaining();
        const bool terminal = left.count() <= 0;
        if (terminal && !first)
            return fail(ctx && ctx->IsKilled() ? -ECANCELED : -ETIMEDOUT);
        first = false;
        ERR_clear_error();
        errno = 0;
        int result = SSL_shutdown(m_ssl);
        int savedErrno = errno;
        int error = result < 0 ? SSL_get_error(m_ssl, result) : SSL_ERROR_NONE;
        if (m_buffer)
        {
            while (BIO_ctrl_pending(m_wbio))
            {
                int count = BIO_read(m_wbio, m_buffer, m_bufferSize);
                if (count <= 0) return fail(-EIO);
                int offset = 0;
                while (offset < count)
                {
                    left = remaining();
                    if (left.count() <= 0 && !terminal)
                        return fail(ctx && ctx->IsKilled()
                                    ? -ECANCELED : -ETIMEDOUT);
                    int sent;
                    if (terminal)
                    {
                        sent = ::send(m_desc.m_fd, m_buffer + offset, count - offset,
                                      MSG_DONTWAIT | MSG_NOSIGNAL);
                        if (sent != count - offset)
                            return fail(sent < 0 ? -errno : -EAGAIN);
                    }
                    else
                        sent = io::SendKill(m_desc, m_buffer + offset, count - offset,
                                            MSG_NOSIGNAL, left);
                    if (sent <= 0) return fail(sent < 0 ? sent : -EPIPE);
                    offset += sent;
                }
            }
        }
        // SSL_shutdown returns zero once our alert was sent: the other half of its
        // two-phase handshake would wait for the peer and is deliberately omitted.
        //
        if (result >= 0) return 0;
        if (terminal)
            return fail(error == SSL_ERROR_SYSCALL && savedErrno ? -savedErrno : -ECANCELED);
        if (error != SSL_ERROR_WANT_WRITE)
            return fail(error == SSL_ERROR_SYSCALL && savedErrno ? -savedErrno : -EIO);
        if (m_buffer) return fail(-EPROTO);
        left = remaining();
        if (left.count() <= 0) continue;
        int ready = io::PollKill(m_desc, POLLOUT, left);
        if (ready < 0) return fail(ready);
    }
}

// Drive the socket BIO handshake cooperatively using readiness waits. OpenSSL operates on the real
// fd, so WANT_READ/WANT_WRITE mean "the socket isn't ready" — we wait for
// readability/writability via io_uring poll, then retry.
//
// After successful handshake, probes for kTLS activation. If the kernel accepted the cipher
// state, subsequent Send/Recv bypass OpenSSL entirely.
//
int Connection::HandshakeSocketBio(bool killAware)
{
    SPDLOG_DEBUG("ssl socket-bio handshake start fd={}", m_desc.m_fd);
    for (;;)
    {
        int ret = SSL_do_handshake(m_ssl);
        if (ret == 1)
        {
            // Probe kTLS activation
            //
            m_ktlsTx = BIO_get_ktls_send(SSL_get_wbio(m_ssl)) != 0;
            m_ktlsRx = BIO_get_ktls_recv(SSL_get_rbio(m_ssl)) != 0;
            SPDLOG_DEBUG("ssl socket-bio handshake complete fd={} ktls_tx={} ktls_rx={}",
                m_desc.m_fd, m_ktlsTx, m_ktlsRx);
            return 0;
        }

        int err = SSL_get_error(m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_READ:
        {
            SPDLOG_TRACE("ssl socket-bio handshake fd={} WANT_READ", m_desc.m_fd);
            int r = killAware ? io::PollKill(m_desc, POLLIN) : io::Poll(m_desc, POLLIN);
            if (r < 0)
            {
                spdlog::warn("ssl socket-bio handshake fd={} poll failed={}", m_desc.m_fd, r);
                return -1;
            }
            break;
        }

        case SSL_ERROR_WANT_WRITE:
        {
            SPDLOG_TRACE("ssl socket-bio handshake fd={} WANT_WRITE", m_desc.m_fd);
            int r = killAware ? io::PollKill(m_desc, POLLOUT) : io::Poll(m_desc, POLLOUT);
            if (r < 0)
            {
                spdlog::warn("ssl socket-bio handshake fd={} poll failed={}", m_desc.m_fd, r);
                return -1;
            }
            break;
        }

        default:
            spdlog::error("ssl socket-bio handshake fd={} failed err={}", m_desc.m_fd, err);
            return -1;
        }
    }
}

// Drive the memory BIO handshake to completion. Loops calling SSL_do_handshake(), shuttling
// ciphertext between the memory BIOs and the descriptor via FlushWrite/FeedRead on each
// WANT_READ or WANT_WRITE cycle.
//
// This is naturally cooperative because io::Send and io::Recv yield the Context while waiting
// for uring completions, so other Contexts run during the handshake's network round-trips.
//
int Connection::HandshakeMemoryBio(bool killAware)
{
    SPDLOG_DEBUG("ssl handshake start fd={}", m_desc.m_fd);
    for (;;)
    {
        int ret = SSL_do_handshake(m_ssl);
        if (ret == 1)
        {
            // Handshake complete. Flush any final output (e.g. Finished message).
            //
            SPDLOG_DEBUG("ssl handshake complete fd={}", m_desc.m_fd);
            return FlushWrite(killAware);
        }

        int err = SSL_get_error(m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_WRITE:
            SPDLOG_TRACE("ssl handshake fd={} WANT_WRITE", m_desc.m_fd);
            if (FlushWrite(killAware) < 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_WANT_READ:
            // Flush first — the handshake may have produced output before asking for input.
            //
            SPDLOG_TRACE("ssl handshake fd={} WANT_READ", m_desc.m_fd);
            if (FlushWrite(killAware) < 0)
            {
                return -1;
            }
            if (FeedRead(killAware) <= 0)
            {
                return -1;
            }
            break;

        default:
            spdlog::error("ssl handshake fd={} failed err={}", m_desc.m_fd, err);
            return -1;
        }
    }
}

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
