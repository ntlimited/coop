#pragma once

#include "context.h"
#include "connection.h"
#include "recv.h"
#include "send.h"
#include "stream.h"

// coop::io::ssl provides a TLS layer on top of coop's io module. It uses OpenSSL 3.x with memory
// BIOs to bridge the TLS state machine with io_uring — all socket I/O flows through the same
// cooperative uring path as plaintext operations, so Contexts yield transparently during TLS
// round-trips just as they do for regular io::Send/io::Recv.
//
// The main types are:
//
//   ssl::Context    — Shared TLS configuration (certificates, keys, protocol). One per listener or
//                     client configuration. Wraps SSL_CTX.
//
//   ssl::Connection — Per-connection TLS state. Wraps an OpenSSL SSL session and a pair of memory
//                     BIOs. Created from a Context and an io::Descriptor.
//
// Free functions mirror the io module's API:
//
//   ssl::Send(connection, buf, size)  — encrypt and send plaintext
//   ssl::Recv(connection, buf, size)  — receive and decrypt ciphertext
//
// Usage:
//
//   // Setup (once per listener)
//   ssl::Context sslCtx(ssl::Mode::Server);
//   sslCtx.LoadCertificateFile("cert.pem");
//   sslCtx.LoadPrivateKeyFile("key.pem");
//
//   // Per connection (after io::Accept returns an fd)
//   io::Descriptor desc(fd);
//   ssl::Connection conn(sslCtx, desc);
//   conn.Handshake();
//
//   // Then use like regular io, just with ssl:: prefix
//   int n = ssl::Recv(conn, buf, sizeof(buf));
//   ssl::Send(conn, buf, n);
//
