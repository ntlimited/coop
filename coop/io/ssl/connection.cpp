#include "connection.h"

#include <cassert>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <spdlog/spdlog.h>

#include "context.h"
#include "coop/io/descriptor.h"
#include "coop/io/poll.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"

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

    // The socket must be non-blocking for cooperative handshake via io::Poll.
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

// Drain any pending data from OpenSSL's write BIO and send it over the wire via io::Send. This
// must be called after every SSL operation that might produce output (handshake, SSL_write, etc).
//
// Returns 0 on success, negative on I/O error.
//
int Connection::FlushWrite()
{
    while (BIO_ctrl_pending(m_wbio) > 0)
    {
        int n = BIO_read(m_wbio, m_buffer, m_bufferSize);
        if (n <= 0)
        {
            break;
        }
        spdlog::trace("ssl flush_write fd={} wbio_bytes={}", m_desc.m_fd, n);

        int at = 0;
        while (at < n)
        {
            int sent = io::Send(m_desc, &m_buffer[at], n - at);
            if (sent <= 0)
            {
                spdlog::warn("ssl flush_write fd={} send failed={}", m_desc.m_fd, sent);
                return -1;
            }
            at += sent;
        }
    }
    return 0;
}

// Read ciphertext from the wire via io::Recv and feed it into OpenSSL's read BIO. Called when
// SSL needs more input data (SSL_ERROR_WANT_READ).
//
// Returns bytes fed on success, 0 on clean disconnect, negative on error.
//
int Connection::FeedRead()
{
    int n = io::Recv(m_desc, m_buffer, m_bufferSize);
    if (n <= 0)
    {
        spdlog::trace("ssl feed_read fd={} recv={}", m_desc.m_fd, n);
        return n;
    }

    int written = BIO_write(m_rbio, m_buffer, n);
    assert(written == n);
    spdlog::trace("ssl feed_read fd={} recv={} rbio_fed={}", m_desc.m_fd, n, written);
    return written;
}

// Dispatch to the appropriate handshake implementation based on the BIO mode.
//
int Connection::Handshake()
{
    if (m_buffer == nullptr)
    {
        return HandshakeSocketBio();
    }
    return HandshakeMemoryBio();
}

// Drive the socket BIO handshake cooperatively using io::Poll. OpenSSL operates on the real fd,
// so WANT_READ/WANT_WRITE mean "the socket isn't ready" — we wait for readability/writability
// via io_uring poll, then retry.
//
// After successful handshake, probes for kTLS activation. If the kernel accepted the cipher
// state, subsequent Send/Recv bypass OpenSSL entirely.
//
int Connection::HandshakeSocketBio()
{
    spdlog::debug("ssl socket-bio handshake start fd={}", m_desc.m_fd);
    for (;;)
    {
        int ret = SSL_do_handshake(m_ssl);
        if (ret == 1)
        {
            // Probe kTLS activation
            //
            m_ktlsTx = BIO_get_ktls_send(SSL_get_wbio(m_ssl)) != 0;
            m_ktlsRx = BIO_get_ktls_recv(SSL_get_rbio(m_ssl)) != 0;
            spdlog::debug("ssl socket-bio handshake complete fd={} ktls_tx={} ktls_rx={}",
                m_desc.m_fd, m_ktlsTx, m_ktlsRx);
            return 0;
        }

        int err = SSL_get_error(m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_READ:
        {
            spdlog::trace("ssl socket-bio handshake fd={} WANT_READ", m_desc.m_fd);
            int r = io::Poll(m_desc, POLLIN);
            if (r < 0)
            {
                spdlog::warn("ssl socket-bio handshake fd={} poll failed={}", m_desc.m_fd, r);
                return -1;
            }
            break;
        }

        case SSL_ERROR_WANT_WRITE:
        {
            spdlog::trace("ssl socket-bio handshake fd={} WANT_WRITE", m_desc.m_fd);
            int r = io::Poll(m_desc, POLLOUT);
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
int Connection::HandshakeMemoryBio()
{
    spdlog::debug("ssl handshake start fd={}", m_desc.m_fd);
    for (;;)
    {
        int ret = SSL_do_handshake(m_ssl);
        if (ret == 1)
        {
            // Handshake complete. Flush any final output (e.g. Finished message).
            //
            spdlog::debug("ssl handshake complete fd={}", m_desc.m_fd);
            return FlushWrite();
        }

        int err = SSL_get_error(m_ssl, ret);
        switch (err)
        {
        case SSL_ERROR_WANT_WRITE:
            spdlog::trace("ssl handshake fd={} WANT_WRITE", m_desc.m_fd);
            if (FlushWrite() < 0)
            {
                return -1;
            }
            break;

        case SSL_ERROR_WANT_READ:
            // Flush first — the handshake may have produced output before asking for input.
            //
            spdlog::trace("ssl handshake fd={} WANT_READ", m_desc.m_fd);
            if (FlushWrite() < 0)
            {
                return -1;
            }
            if (FeedRead() <= 0)
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
