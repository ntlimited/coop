# TLS client authentication

A TLS connection needs three independent decisions: which certificates to trust, which peer
identity to verify, and which virtual server to request through SNI. coop exposes those decisions
explicitly. Constructing a client `ssl::Context` retains OpenSSL's `SSL_VERIFY_NONE` default;
loading a trust anchor or setting an identity does not enable verification by itself.

Configure a shared context before creating its connections. A caller with an in-memory trust
anchor can avoid filesystem I/O entirely:

```cpp
#include "coop/io/ssl/context.h"
#include "coop/io/ssl/connection.h"

bool ConfigureClient(coop::io::ssl::Context& ctx, const char* caPem, size_t caLen)
{
    if (!ctx.AddTrustedCertificate(caPem, caLen))
    {
        return false;
    }
    ctx.EnablePeerVerification();
    return true;
}
```

`AddTrustedCertificate` accepts one PEM certificate and optional trailing whitespace. Call it
once per trust anchor; a failed call does not partially import a bundle. The PEM storage can be
released after the call. Applications with their own OpenSSL certificate store can configure
`SSL_CTX_get_cert_store(ctx.m_ctx)` directly.

To use OpenSSL's default trust locations instead, explicitly call and check
`ctx.LoadDefaultVerifyPaths()` during application setup, outside a cooperator. OpenSSL may
perform blocking filesystem I/O, including deferred directory lookups during verification.
A successful setup result does not prove that a usable trust anchor exists. The helper does
not enable verification; call `EnablePeerVerification()` separately.

For a DNS peer, set both SNI and the expected identity before handshaking. The descriptor is an
existing connection owned and connected by the caller; the context outlives its TLS connections:

```cpp
bool UseDnsPeer(coop::io::ssl::Context& verifiedCtx, coop::io::Descriptor& descriptor,
                const char* hostname)
{
    char staging[coop::io::ssl::Connection::BUFFER_SIZE];
    coop::io::ssl::Connection tls(verifiedCtx, descriptor, staging, sizeof(staging));
    if (!tls.SetServerName(hostname) || !tls.SetVerifyHost(hostname))
    {
        return false;
    }
    if (tls.Handshake() < 0)
    {
        // SSL_get_verify_result(tls.m_ssl) distinguishes certificate verification failures;
        // other handshake failures can be transport or protocol errors.
        return false;
    }
    // Consume the TLS connection here, while tls, descriptor, and staging remain alive.
    return true;
}
```

SNI selects a virtual server; it does not authenticate it. For an IP endpoint, use
`tls.SetVerifyIp("127.0.0.1")` or `tls.SetVerifyIp("::1")` and omit SNI. IP matching checks the
certificate's IP subjectAltName entries. Inputs are unbracketed literals without a port or zone
identifier. When dialing a known address for a DNS service, authenticate the service's DNS name
and send its SNI name; the routing address and authenticated identity need not be identical.

All identity setters return checked results, copy their inputs into OpenSSL configuration, and
perform no I/O. Null and empty inputs fail without clearing an existing identity; malformed IP
literals also fail. DNS setters use OpenSSL's input and matching rules rather than imposing a
second DNS policy. Choose one verification identity kind on each fresh connection: these helpers
do not clear other identities or verification parameters previously installed on `m_ssl`.

The helpers add no object state or branches to send/receive operations. Raw `m_ctx` and `m_ssl`
remain available for per-connection verification, custom trust stores or verification callbacks,
explicit verification disabling, hostname flags, protocol/cipher configuration, and ALPN. Existing
OpenSSL policies are otherwise preserved. `EnablePeerVerification()` selects `SSL_VERIFY_PEER`
and OpenSSL's default callback; install a custom callback afterward when needed. Mutating a
context does not retroactively configure existing connections.

The same configuration works with `SocketBio` and optional kTLS. It does not change handshake,
deadline, or cancellation behavior: callers still choose `Handshake()` or `HandshakeKill()`.

The `TlsClientConfigTest.*` tests drive actual TLS records between memory BIOs. They cover trusted
and untrusted peers, DNS and IPv4/IPv6 identity checks, SNI, explicit verification policy, caller
buffer lifetimes, and rejected configuration input. They do not initialize io_uring or use a
network socket:

```sh
./build/debug/bin/coop_tls_config_tests
```

See `examples/http_fetch.cpp` for an executable native HTTP/1.1 client that combines
these explicit setup operations with borrowed body reads and checked completion.
