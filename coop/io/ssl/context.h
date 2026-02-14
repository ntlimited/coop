#pragma once

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
//     ssl::Context ctx(ssl::Mode::Server);
//     ctx.LoadCertificate("cert.pem");
//     ctx.LoadPrivateKey("key.pem");
//
//     // Then for each accepted connection:
//     ssl::Connection conn(ctx, descriptor);
//
struct Context
{
    Context(Context const&) = delete;
    Context(Context&&) = delete;

    Context(Mode mode);
    ~Context();

    // Load a PEM-encoded certificate file. Returns true on success.
    //
    bool LoadCertificate(const char* path);

    // Load a PEM-encoded private key file. Returns true on success.
    //
    bool LoadPrivateKey(const char* path);

    SSL_CTX* m_ctx;
    Mode     m_mode;
};

} // end namespace coop::io::ssl
} // end namespace coop::io
} // end namespace coop
