#pragma once

#include "context.h"
#include "connection.h"
#include "recv.h"
#include "send.h"
#include "sendfile.h"
#include "stream.h"

// coop::io::ssl provides a TLS layer on top of coop's io module. Two modes are supported:
//
// **Memory BIO mode**: OpenSSL uses memory BIOs, all socket I/O flows through io_uring via staging
// buffers. Works with any socket type (TCP, AF_UNIX). Default mode.
//
// **Socket BIO mode** (via `ssl::SocketBio` tag): OpenSSL operates on the real fd. After handshake,
// if kTLS activates, Send/Recv bypass OpenSSL entirely — the kernel handles crypto. Requires TCP.
// Falls back to SSL_write/SSL_read + io::Poll if kTLS isn't available.
//
// The main types are:
//
//   ssl::Context    — Shared TLS configuration (certificates, keys, protocol). One per listener or
//                     client configuration. Wraps SSL_CTX. Call EnableKTLS() for kTLS support.
//
//   ssl::Connection — Per-connection TLS state. Memory BIO ctor takes a staging buffer; socket BIO
//                     ctor takes `ssl::SocketBio{}` tag (no buffer needed).
//
// Free functions mirror the io module's API:
//
//   ssl::Send(connection, buf, size)  — encrypt and send plaintext
//   ssl::Recv(connection, buf, size)  — receive and decrypt ciphertext
//
// Usage (memory BIO):
//
//   ssl::Connection conn(sslCtx, desc, buffer, sizeof(buffer));
//   conn.Handshake();
//   ssl::Send(conn, buf, n);
//
// Usage (kTLS):
//
//   sslCtx.EnableKTLS();
//   ssl::Connection conn(sslCtx, desc, ssl::SocketBio{});
//   conn.Handshake();  // probes kTLS after handshake
//   ssl::Send(conn, buf, n);  // bypasses OpenSSL if kTLS active
//
