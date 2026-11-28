#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <sys/socket.h>

#include "coop/time/interval.h"
#include "coop/http/connection.h"

namespace coop
{

struct Cooperator;
struct Context;

namespace io { namespace ssl { struct Context; } }

namespace http
{

struct ConnectionBase;
struct ServerHandle;

// The request handler owns dispatch entirely. coop parses the request line and headers, then hands
// the connection to this one callback -- it inspects conn.GetRequestLine()->path and writes a
// response however it likes. coop does no path matching, no static-file fallback, and no default
// 404 of its own: routing is the application's concern, not the library's. userData is passed
// through verbatim from ServerConfiguration for handlers that need per-server state.
//
using RequestHandler = void (*)(ConnectionBase& conn, void* userData);

// Optional per-request admission before any dispatch. Return 0 to accept, or 400..599
// to send a refusal and close. Other statuses refuse with 500. The validated head is
// borrowed only through the callback; inspection preserves normal query/header/body iteration.
// The callback runs on the serving context and may yield. userData is the server's.
//
using RequestAdmissionHandler = int (*)(ConnectionBase& conn, const RequestHead& head,
                                       void* userData);

// Connection hooks run on the serving context, including TLS handshake failures. Open may
// allocate application-owned state and refuse the connection by returning false. Close runs
// once even after refusal, after handlers and attached descendant contexts finish. Detached
// contexts require their own lifetime contract. The library never frees application state.
//
// peer points to a socket address borrowed only for the duration of the open callback.
// Copy it to retain it. A failed address query supplies nullptr with a zero length.
//
using ConnectionOpenHandler = bool (*)(Context* ctx, const sockaddr* peer, socklen_t peerLength,
                                       void* userData, void*& state);
using ConnectionCloseHandler = void (*)(Context* ctx, void* userData, void* state);

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

    // Select the local IPv4 interface; the default accepts on every interface.
    // Borrowed null-terminated numeric address, retained through Run*Server.
    // Null or malformed addresses refuse startup rather than falling back to wildcard.
    //
    const char* bindAddress = "0.0.0.0";

    int backlog = 512;
    bool reusePort = true;
    bool multishotAccept = false;
    uint32_t maxPendingAccepts = 64;

    // Parse requests from provided-buffer-ring chunks (multishot recv) instead of a
    // per-connection one-shot recv. Requires the cooperator's uring to carry a
    // BufferRing (UringConfiguration::bufferRingEntries); silently classic otherwise.
    // Plaintext connections only — TLS bytes need the SSL layer's decrypt path, and
    // splice-to-disk bodies bounce in this mode (armed recv already drained the socket).
    //
    bool pbufRecv = false;

    // Request parser limits and malformed-request response policy.
    //
    ServerParserOptions parserOptions{};

    // Graceful-drain control. When set, connections detach from the acceptor and
    // register with this handle so ServerHandle::Drain can stop accepting and drain
    // in-flight connections without cascade-killing them. nullptr = no drain machinery,
    // identical to the pre-drain behavior.
    //
    ServerHandle* control = nullptr;

    // The one request handler and its pass-through state. handler must be non-null: RunServer logs
    // and returns false otherwise.
    //
    RequestHandler handler = nullptr;
    void* userData = nullptr;

    // The head snapshot expires immediately after the callback; routing uses ordinary
    // parsing. Snapshot/replay allocations are freed by header completion.
    // Head capture/validation is entirely opt-in. The cap counts request-line plus
    // header wire bytes; oversized heads receive 431 before admission or dispatch.
    //
    RequestAdmissionHandler requestAdmission = nullptr;
    size_t maxRequestHeadBytes = 16384;

    // Optional connection-lifetime state, separate from the per-server userData. Open runs
    // before TLS handshake/request parsing; close receives its output even when open refuses.
    // Both hooks may yield. State remains borrowed by ConnectionBase::GetConnectionState().
    // Hook functions and userData must outlive active connections, including detached drain.
    //
    ConnectionOpenHandler connectionOpen = nullptr;
    ConnectionCloseHandler connectionClose = nullptr;

    const char* name = "HttpServer";
    time::Interval timeout = std::chrono::seconds(30);
};

// Run an HTTP server. Binds, listens, and accepts connections in a loop, launching a context per
// client that parses requests and calls config.handler. Returns false (with the failure logged)
// when handler is null, bindAddress is invalid, or the socket cannot be created, bound, or
// listened. Callers must check startup success. Multiple callers on one port intentionally shard
// accepts via SO_REUSEPORT.
//
bool RunServer(Context* ctx, ServerConfiguration const& config);

// Run an HTTPS server. Same as RunServer but performs a TLS handshake on each accepted connection
// before entering the HTTP handler loop. sslCtx is borrowed: with control enabled it,
// the handle, and handler state must outlive detached connections (wait for Drain),
// even after RunTlsServer returns. TLS handshakes count as live connections during drain.
//
bool RunTlsServer(Context* ctx, ServerConfiguration const& config, io::ssl::Context& sslCtx);

// Serve a static file matching reqPath from a null-terminated list of search-path roots. A helper
// for handlers that want static serving -- coop does not call it on its own. Returns true if a file
// was found and sent (200 + sendfile), false if no candidate existed (the handler should then send
// its own 404). Rejects paths containing "..".
//
bool ServeFile(ConnectionBase& conn, std::string_view reqPath, const char* const* searchPaths);

} // end namespace coop::http
} // end namespace coop
