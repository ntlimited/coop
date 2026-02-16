#pragma once

#include <cstddef>
#include <openssl/ssl.h>

namespace coop
{

namespace io
{

struct Descriptor;

namespace ssl
{

struct Context;

// Tag type to select the socket BIO constructor. When used, OpenSSL operates on the real socket fd
// instead of memory BIOs. Required for kTLS (kernel TLS offload) — after handshake, the kernel
// handles encryption/decryption transparently so io::Send/io::Recv bypass OpenSSL entirely.
//
struct SocketBio {};

// Connection wraps a single TLS session over an existing io::Descriptor. Two modes:
//
// **Memory BIO mode** (default): Decouples OpenSSL from the socket. All I/O goes through coop's
// io_uring path via staging buffers:
//   Plaintext out:  SSL_write() -> wbio (memory) -> FlushWrite() -> io::Send -> uring -> kernel
//   Plaintext in:   kernel -> uring -> io::Recv -> FeedRead() -> rbio (memory) -> SSL_read()
//
// **Socket BIO mode** (SocketBio tag): OpenSSL operates on the real fd. Handshake uses io::Poll
// for cooperative waiting. If kTLS activates, Send/Recv bypass OpenSSL on the data path entirely.
// Falls back to SSL_write/SSL_read + io::Poll if kTLS doesn't activate.
//
struct Connection
{
    Connection(Connection const&) = delete;
    Connection(Connection&&) = delete;

    // Recommended minimum buffer size for the I/O staging buffer (memory BIO mode only).
    //
    static constexpr size_t BUFFER_SIZE = 16384;

    // Memory BIO constructor — requires caller-provided staging buffer.
    //
    Connection(Context& ctx, Descriptor& desc, char* buffer, size_t bufferSize);

    // Socket BIO constructor — OpenSSL operates on the real fd. No staging buffer needed.
    // Enables kTLS if the ssl::Context has EnableKTLS() set and the kernel supports it.
    //
    Connection(Context& ctx, Descriptor& desc, SocketBio);

    ~Connection();

    // Perform the TLS handshake. Dispatches to the appropriate handshake implementation based
    // on the BIO mode. Returns 0 on success, negative on error.
    //
    int Handshake();

    SSL*         m_ssl;
    Descriptor&  m_desc;

    // kTLS activation flags — set after successful socket BIO handshake. When true, Send/Recv
    // bypass OpenSSL and use io::Send/io::Recv directly (kernel handles crypto).
    //
    bool m_ktlsTx = false;
    bool m_ktlsRx = false;

  private:
    friend int Send(Connection&, const void*, size_t);
    friend int SendAll(Connection&, const void*, size_t);
    friend int Recv(Connection&, void*, size_t);

    // Socket BIO handshake — drives SSL_do_handshake with io::Poll for cooperative waiting.
    // After completion, probes for kTLS activation and sets m_ktlsTx/m_ktlsRx.
    //
    int HandshakeSocketBio();

    // Memory BIO handshake — drives SSL_do_handshake via FlushWrite/FeedRead.
    //
    int HandshakeMemoryBio();

    // Push any pending encrypted data from OpenSSL's write BIO out through the descriptor. Called
    // after any SSL operation that may produce output (handshake steps, SSL_write, shutdown).
    // Memory BIO mode only.
    //
    int FlushWrite();

    // Pull encrypted data from the descriptor into OpenSSL's read BIO. Called when an SSL operation
    // returns SSL_ERROR_WANT_READ, meaning it needs more ciphertext to proceed.
    // Memory BIO mode only.
    //
    int FeedRead();

    BIO* m_rbio;
    BIO* m_wbio;

    char* m_buffer;
    size_t m_bufferSize;
};

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
