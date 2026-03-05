#pragma once

#include <cstdint>

#include "coop/time/interval.h"

namespace coop
{

struct Cooperator;
struct Context;

namespace io { namespace ssl { struct Context; } }

namespace http
{

struct ConnectionBase;

struct Route
{
    const char* path;
    void (*handler)(ConnectionBase&);
};

// Run an HTTP server on the given port with the provided route table. Binds, listens, and accepts
// connections in a loop, launching a handler context per client.
//
void RunServer(
    Context* ctx,
    int port,
    const Route* routes,
    int routeCount,
    const char* name = "HttpServer",
    const char* const* searchPaths = nullptr,
    time::Interval timeout = std::chrono::seconds(30));

// Run an HTTPS server. Same as RunServer but performs a TLS handshake on each accepted connection
// before entering the HTTP handler loop. Uses socket BIO mode with kTLS when available.
//
void RunTlsServer(
    Context* ctx,
    int port,
    const Route* routes,
    int routeCount,
    io::ssl::Context& sslCtx,
    const char* name = "HttpsServer",
    const char* const* searchPaths = nullptr,
    time::Interval timeout = std::chrono::seconds(30));

} // end namespace coop::http
} // end namespace coop
