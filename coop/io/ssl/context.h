#pragma once

#include <cstddef>
#include <openssl/ssl.h>

namespace coop
{

namespace io
{

namespace ssl
{

// Mode determines whether this context will be used for accepting (Server) or initiating (Client)
// TLS connections. This selects the appropriate OpenSSL method (TLS_server_method vs
// TLS_client_method) at construction time.
//
enum class Mode : uint8_t
{
    Server,
    Client,
};

// Context wraps an OpenSSL SSL_CTX, which holds shared TLS configuration (certificates, keys,
// protocol settings) for all connections created from it. Typically one Context is created per
// listening port or client configuration and then shared across connections.
//
// Usage:
//
//     // From PEM buffers (no file I/O):
//     ssl::Context ctx(ssl::Mode::Server);
//     ctx.LoadCertificate(pemData, pemLen);
//     ctx.LoadPrivateKey(keyData, keyLen);
//
//     // Then for each accepted connection:
//     ssl::Connection conn(ctx, descriptor, buffer, bufferSize);
//
struct Context
{
    Context(Context const&) = delete;
    Context(Context&&) = delete;

    Context(Mode mode);
    ~Context();

    // Load a PEM-encoded certificate chain from memory. Returns true on success.
    //
    bool LoadCertificate(const char* pem, size_t len);

    // Load a PEM-encoded private key from memory. Returns true on success.
    //
    bool LoadPrivateKey(const char* pem, size_t len);

    // Client authentication policy is opt-in: construction retains OpenSSL's SSL_VERIFY_NONE
    // default. Configure shared policy before creating connections. This enables chain
    // verification with OpenSSL's default callback; set a DNS/IP identity on each connection
    // as well. m_ctx remains available for custom callbacks and other verification policies.
    //
    void EnablePeerVerification();

    // Explicitly load OpenSSL's default trust paths. May perform blocking filesystem I/O;
    // call during application setup, outside a cooperator. Directory-backed stores can also
    // perform deferred filesystem lookups during verification; use memory anchors to avoid
    // this. Returns OpenSSL's setup result, which does not guarantee usable anchors were found.
    //
    [[nodiscard]] bool LoadDefaultVerifyPaths();

    // Add one PEM-encoded trust anchor from memory, with optional trailing whitespace.
    // No file I/O. Does not enable verification. OpenSSL retains its own certificate reference;
    // the input buffer can be released on return. False leaves the trust store unchanged.
    // For bundles/custom stores, use SSL_CTX_get_cert_store(m_ctx) directly.
    //
    [[nodiscard]] bool AddTrustedCertificate(const char* pem, size_t len);

    // Enable kernel TLS offload. When active, OpenSSL installs cipher state into the kernel
    // after handshake so that subsequent send/recv bypasses userspace crypto entirely. Requires
    // a TCP socket (not AF_UNIX) and a socket BIO connection. Must be called before any
    // connections are created from this context.
    //
    void EnableKTLS();

    SSL_CTX* m_ctx;
    Mode     m_mode;
};

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
