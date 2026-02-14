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

// Connection wraps a single TLS session over an existing io::Descriptor. It uses OpenSSL's memory
// BIO mechanism to decouple the TLS state machine from the underlying socket, allowing all actual
// I/O to flow through coop's io_uring path.
//
// The bridging works as follows:
//
//   Plaintext out:  SSL_write() -> wbio (memory) -> FlushWrite() -> io::Send -> uring -> kernel
//   Plaintext in:   kernel -> uring -> io::Recv -> FeedRead() -> rbio (memory) -> SSL_read()
//
// Because FlushWrite and FeedRead use io::Send and io::Recv internally, all blocking is cooperative
// — the current Context yields while waiting for uring completions, and other Contexts run in the
// meantime. No special scheduling is needed beyond what the io module already provides.
//
struct Connection
{
    Connection(Connection const&) = delete;
    Connection(Connection&&) = delete;

    // Recommended minimum buffer size for the I/O staging buffer.
    //
    static constexpr size_t BUFFER_SIZE = 16384;

    Connection(Context& ctx, Descriptor& desc, char* buffer, size_t bufferSize);
    ~Connection();

    // Perform the TLS handshake. Server-mode connections call SSL_accept semantics; client-mode
    // connections call SSL_connect semantics. Both are abstracted by SSL_do_handshake internally.
    //
    // Returns 0 on success, negative on error.
    //
    int Handshake();

    SSL*         m_ssl;
    Descriptor&  m_desc;

  private:
    friend int Send(Connection&, const void*, size_t);
    friend int Recv(Connection&, void*, size_t);

    // Push any pending encrypted data from OpenSSL's write BIO out through the descriptor. Called
    // after any SSL operation that may produce output (handshake steps, SSL_write, shutdown).
    //
    int FlushWrite();

    // Pull encrypted data from the descriptor into OpenSSL's read BIO. Called when an SSL operation
    // returns SSL_ERROR_WANT_READ, meaning it needs more ciphertext to proceed.
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
