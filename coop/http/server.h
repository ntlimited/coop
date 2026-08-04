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

// Declared server topology and accept policy. reusePort is the sharding contract: N
// RunServer calls with the same port — one per cooperator — each get their own listen
// socket and the kernel distributes connections across them; no shared accept fd, no
// cross-cooperator handoff. multishotAccept arms one SQE for the listener's lifetime
// (kernel 5.19+; the one-shot loop otherwise), with maxPendingAccepts bounding
// surfaced-but-unconsumed connections (see io::ArmedAccept's backpressure covenant).
//
struct ServerConfiguration
{
    int port = 8080;
    int backlog = 512;
    bool reusePort = true;
    bool multishotAccept = false;
    uint32_t maxPendingAccepts = 64;
    const char* name = "HttpServer";
    const char* const* searchPaths = nullptr;
    time::Interval timeout = std::chrono::seconds(30);
};

// Run an HTTP server on the given port with the provided route table. Binds, listens, and accepts
// connections in a loop, launching a handler context per client. Returns false (with the failure
// logged) when the socket cannot be created, bound, or listened — callers must not assume the
// server came up. Multiple callers on one port intentionally shard accepts via SO_REUSEPORT.
//
bool RunServer(
    Context* ctx,
    ServerConfiguration const& config,
    const Route* routes,
    int routeCount);

bool RunServer(
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
bool RunTlsServer(
    Context* ctx,
    ServerConfiguration const& config,
    const Route* routes,
    int routeCount,
    io::ssl::Context& sslCtx);

bool RunTlsServer(
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
