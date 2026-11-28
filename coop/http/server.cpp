#include "server.h"
#include "connection.h"
#include "transport.h"
#include "tls_transport.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <optional>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "coop/coordinator.h"
#include "coop/io/armed_accept.h"
#include "coop/io/buffer_ring.h"
#include "coop/io/recv_source.h"
#include "server_handle.h"
#include "coop/io/uring.h"
#include "coop/io/shutdown_on_kill.h"

#include "coop/alloc.h"
#include "coop/cooperator.h"
#include "coop/launchable.h"
#include "coop/io/io.h"
#include "coop/io/ssl/connection.h"
#include "coop/io/ssl/context.h"
#include "coop/self.h"
#include "coop/time/now.h"

namespace coop
{
namespace http
{

namespace
{

const char* ContentTypeForExtension(const char* path)
{
    const char* dot = strrchr(path, '.');
    if (!dot)
    {
        return "application/octet-stream";
    }

    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0)  return "text/css";
    if (strcmp(dot, ".js") == 0)   return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";

    return "application/octet-stream";
}

bool HasPathTraversal(const char* path)
{
    return strstr(path, "..") != nullptr;
}

} // end anonymous namespace

// Public static-file helper (declared in server.h). A handler opts into static serving by calling
// it; coop never calls it on its own. ContentTypeForExtension / HasPathTraversal above stay
// internal to this TU but remain visible here.
//
bool ServeFile(ConnectionBase& conn, std::string_view reqPath,
               const char* const* searchPaths)
{
    // Path traversal check — reqPath is a string_view, need null-terminated copy
    //
    char pathBuf[512];
    if (reqPath.size() >= sizeof(pathBuf)) return false;
    memcpy(pathBuf, reqPath.data(), reqPath.size());
    pathBuf[reqPath.size()] = '\0';

    if (HasPathTraversal(pathBuf)) return false;

    const char* uriPath = pathBuf;
    if (reqPath == "/")
    {
        uriPath = "/index.html";
    }

    char filePath[512];

    for (const char* const* sp = searchPaths; *sp != nullptr; sp++)
    {
        int len = snprintf(filePath, sizeof(filePath), "%s%s", *sp, uriPath);
        if (len < 0 || static_cast<size_t>(len) >= sizeof(filePath))
        {
            continue;
        }

        int fileFd = ::open(filePath, O_RDONLY);
        if (fileFd < 0) continue;

        struct stat st;
        if (::fstat(fileFd, &st) != 0 || !S_ISREG(st.st_mode))
        {
            ::close(fileFd);
            continue;
        }

        const char* ct = ContentTypeForExtension(filePath);
        conn.SendHeaders(200, ct, st.st_size);

        if (st.st_size > 0)
        {
            conn.Sendfile(fileFd, 0, st.st_size);
        }

        ::close(fileFd);
        return true;
    }

    return false;
}

namespace
{

// The registration outlives the descriptor member: Drain observes zero live connections
// only after parser, TLS and descriptor teardown have completed (destructors may yield).
//
struct DrainRegistration
{
    ServerHandle* control;
    ServerHandle::ConnNode node;
    bool registered = false;

    explicit DrainRegistration(ServerHandle* value) : control(value) {}
    void Begin(Context* ctx, io::Descriptor* desc)
    {
        if (!control) return;
        ctx->Detach();
        control->Register(&node, desc);
        registered = true;
    }
    ~DrainRegistration() { if (registered) control->Deregister(&node); }
};

// Remove the parser pointer before its allocation unwinds. TLS/descriptor teardown can
// suspend, and a concurrent drain must never call ForceClose on an expired parser.
//
struct DrainParser
{
    ServerHandle::ConnNode* node;
    DrainParser(DrainRegistration& registration, ConnectionBase* connection)
        : node(registration.control ? &registration.node : nullptr)
    {
        if (node) node->connection = connection;
    }
    ~DrainParser() { if (node) { node->connection = nullptr; node->idle = false; } }
};

// Application state outlives every attached context that may borrow it. Context's normal
// descendant join follows task-local destruction, so serving connections perform that
// same retirement join explicitly while the parser and transport resources are still alive.
//
struct ConnectionLifetime
{
    Context* ctx;
    io::Descriptor& descriptor;
    ConnectionOpenHandler open;
    ConnectionCloseHandler close;
    void* userData;
    void* state = nullptr;
    bool joined = false;

    bool Open()
    {
        if (!open) return true;
        sockaddr_storage peer{};
        socklen_t length = sizeof(peer);
        const sockaddr* address = nullptr;
        if (::getpeername(descriptor.m_fd, reinterpret_cast<sockaddr*>(&peer), &length) == 0 &&
            length <= sizeof(peer))
            address = reinterpret_cast<const sockaddr*>(&peer);
        else
            length = 0;
        return open(ctx, address, length, userData, state);
    }

    void JoinChildren()
    {
        if (joined) return;
        // Children may borrow application state through cleanup. Cancel and join them
        // without marking a healthy serving context killed or shutting down its socket.
        //
        KillCause cause = ctx->IsKilled() ? ctx->WhyKilled() : KillCause::Kill;
        ctx->JoinChildren(cause);
        joined = true;
    }

    ~ConnectionLifetime()
    {
        JoinChildren();
        if (close) close(ctx, userData, state);
    }
};

// Declared after the connection's borrowed resources, so their teardown follows the join.
//
struct ConnectionChildren
{
    ConnectionLifetime& lifetime;
    ~ConnectionChildren() { lifetime.JoinChildren(); }
};

// A refusal must reach a peer that is still uploading. Half-close the response before
// discarding a bounded remainder; closing immediately with unread socket data can reset
// the connection before the peer observes the response. The parser never sees these bytes.
//
struct RefusalClose
{
    ConnectionBase& connection;
    io::Descriptor& descriptor;

    ~RefusalClose()
    {
        if ((!connection.ParseError() && !connection.RequestRejected()) ||
            connection.SendError()) return;
        // A truncated streamed response has already selected an abortive close. A
        // half-close here could expose a clean FIN before that reset reaches the peer.
        //
        linger setting{};
        socklen_t length = sizeof(setting);
        if (::getsockopt(descriptor.m_fd, SOL_SOCKET, SO_LINGER, &setting, &length) == 0 &&
            setting.l_onoff && setting.l_linger == 0) return;
        if (::shutdown(descriptor.m_fd, SHUT_WR) != 0) return;

        // Giving up with input still queued would reset this socket on close, even
        // after SHUT_RD. Discard one queue snapshot without copying or waiting; a
        // continuously uploading peer cannot extend this final cleanup operation.
        //
        struct DiscardQueued
        {
            int fd;
            ~DiscardQueued()
            {
                ::shutdown(fd, SHUT_RD);
                int queued = 0;
                if (::ioctl(fd, FIONREAD, &queued) == 0 && queued > 0)
                    ::recv(fd, nullptr, static_cast<size_t>(queued), MSG_DONTWAIT | MSG_TRUNC);
            }
        } discard{descriptor.m_fd};

        static constexpr size_t MAX_DRAIN = 1024 * 1024;
        const int64_t end = time::MonotonicMicros() + 200000;
        auto* ctx = Self();
        char buffer[4096];
        size_t remaining = MAX_DRAIN;
        while (remaining)
        {
            if (ctx && (ctx->IsKilled() || ctx->RemainingUs() <= 0)) return;
            int64_t budget = end - time::MonotonicMicros();
            if (budget <= 0) return;
            int received = io::RecvKill(descriptor, buffer,
                std::min(remaining, sizeof(buffer)), 0, time::Interval(budget));
            if (received <= 0) return;
            remaining -= static_cast<size_t>(received);
        }
    }
};

// A missing stream terminator must not masquerade as a successfully delimited EOF.
// Construct before the child join so attached writers can finish while resources live.
//
template<typename Conn>
struct ResponseClosure
{
    Conn& conn;
    io::Descriptor& descriptor;
    Context* ctx;
    io::ssl::Connection* tls = nullptr;
    time::Interval timeout{};

    ~ResponseClosure()
    {
        if (!conn.CloseDelimitedResponse()) return;
        bool complete = conn.CloseDelimitedResponseComplete();
        if (complete && tls)
        {
            auto bound = timeout.count() > 0 && timeout < std::chrono::seconds(1)
                       ? timeout : std::chrono::seconds(1);
            // Completion is irrevocable: cancellation after EndChunked cannot turn
            // a completed body into an abortive TCP reset. TLS still gets
            // one nonwaiting alert attempt when its context can no longer wait.
            //
            tls->ShutdownWriteKill(bound);
        }
        if (!complete)
        {
            linger abort{1, 0};
            ::setsockopt(descriptor.m_fd, SOL_SOCKET, SO_LINGER, &abort, sizeof(abort));
        }
    }
};

struct ListenerRegistration
{
    ServerHandle* control;
    io::Descriptor* listener;
    ListenerRegistration(ServerHandle* value, io::Descriptor* desc)
        : control(value), listener(desc)
    {
        if (control) control->SetListener(listener);
    }
    ~ListenerRegistration() { if (control) control->ClearListener(listener); }
};

// Serve one request. Returns false when no request could be parsed — clean keep-alive
// EOF or malformed bytes — which must END the connection loop: a clean EOF that keeps
// looping spins hot on instant zero-byte reads (the recv fastpath returns EOF without
// ever parking), monopolizing the cooperator.
//
bool HandleRequest(ConnectionBase& conn, RequestHandler handler, void* userData,
                   ServerHandle::ConnNode* node, RequestAdmissionHandler admission,
                   size_t maxHeadBytes)
{
    if (node) node->idle = true;
    auto* req = conn.GetRequestLine();
    if (node) node->idle = false;
    if (!req)
    {
        if (admission && conn.ParseError() != 0)
        {
            conn.RejectRequest(conn.ParseError());
            return false;
        }
        if (!admission && conn.ParseError() != 0 &&
            conn.GetParserOptions().errorResponse == ParserErrorResponse::Caller)
        {
            handler(conn, userData);
            return false;
        }
        // A clean keep-alive EOF (peer closed between requests) leaves nothing in the
        // buffer — answering it with a 400 writes into a dead socket. Only malformed
        // bytes earn a response.
        //
        if (!conn.SendError() && conn.LeftoverSize() > 0)
        {
            conn.Send(400, "text/plain", "Bad Request\n");
        }
        return false;
    }

    if (admission)
    {
        const auto* head = conn.InspectRequestHead(maxHeadBytes);
        if (node) node->idle = false;
        if (!head) return false;
        int status = admission(conn, *head, userData);
        conn.ReleaseRequestHead();
        if (conn.RequestRejected() || conn.Error()) return false;
        if (status != 0)
        {
            conn.RejectRequest(status);
            return false;
        }
    }

    // Everything past parsing is the application's: matching, static serving, 404s. coop just
    // hands over the parsed connection.
    //
    handler(conn, userData);
    return !conn.RequestRejected();
}

// -------------------------------------------------------------------------------------
// Plaintext HTTP connection handler
// -------------------------------------------------------------------------------------

struct HttpConnection : Launchable
{
    HttpConnection(Context* ctx, int fd, Cooperator* co,
                   RequestHandler handler, void* userData,
                   time::Interval timeout,
                   bool pbufRecv = false,
                   ServerHandle* control = nullptr,
                   ServerParserOptions parserOptions = {},
                   ConnectionOpenHandler connectionOpen = nullptr,
                   ConnectionCloseHandler connectionClose = nullptr,
                   RequestAdmissionHandler requestAdmission = nullptr,
                   size_t maxRequestHeadBytes = 16384)
    : Launchable(ctx)
    , m_registration(control)
    , m_parserOptions(parserOptions)
    , m_connectionOpen(connectionOpen)
    , m_connectionClose(connectionClose)
    , m_requestAdmission(requestAdmission)
    , m_maxRequestHeadBytes(maxRequestHeadBytes)
    , m_fd(fd)
    , m_shutdownGuard(ctx, m_fd, SHUT_RD)
    , m_co(co)
    , m_handler(handler)
    , m_userData(userData)
    , m_timeout(timeout)
    , m_pbufRecv(pbufRecv)
    , m_control(control)
    {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        ctx->SetName("HttpConnection");
    }

    virtual void Launch() final
    {
        m_registration.Begin(GetContext(), &m_fd);
        ConnectionLifetime lifetime{
            GetContext(), m_fd, m_connectionOpen, m_connectionClose, m_userData};
        if (!lifetime.Open()) return;

        using Conn = Connection<PlaintextTransport>;
        PlaintextTransport transport(m_fd);
        auto conn = GetContext()->Allocate<Conn>(
            Conn::ExtraBytes(), transport, GetContext(), m_co,
            ConnectionBase::DEFAULT_BUFFER_SIZE, ConnectionBase::DEFAULT_SEND_BUFFER_SIZE,
            m_timeout, m_parserOptions, lifetime.state);
        DrainParser parserRegistration(m_registration, conn.get());
        RefusalClose refusal{*conn, m_fd};

        // Pbuf mode: one armed multishot recv serves the connection's lifetime; the
        // parser windows over kernel-selected chunks. Falls back to classic recv when
        // the uring carries no buffer ring.
        //
        io::BufferRing* ring = m_pbufRecv ? m_fd.m_ring->GetBufferRing() : nullptr;
        std::optional<io::RecvSource> source;
        if (ring)
        {
            source.emplace(GetContext(), m_fd, ring);
            conn->AttachRecvSource(&*source);
        }

        ResponseClosure<Conn> closure{*conn, m_fd, GetContext()};
        ConnectionChildren children{lifetime};
        while (!GetContext()->IsKilled())
        {
            // Draining: force this response to close the connection, then exit after it.
            //
            if (m_control && m_control->IsDraining())
            {
                conn->ForceClose();
            }
            if (!HandleRequest(*conn, m_handler, m_userData,
                               m_control ? &m_registration.node : nullptr,
                               m_requestAdmission, m_maxRequestHeadBytes)) return;

            if (conn->SendError()) return;
            // A completed close-delimited response cannot be reused. Do not turn
            // later cancellation into a receive failure while draining the request.
            //
            if (conn->CloseDelimitedResponseComplete() &&
                (GetContext()->IsKilled() || GetContext()->RemainingUs() <= 0)) return;

            // Drain the request before judging keep-alive: a handler that never touched
            // the headers hasn't parsed Connection yet, and checking first would let
            // Reset() wipe a close that SkipBody just discovered — a phantom extra
            // iteration against a closed peer.
            //
            if (!conn->SkipBody()) return;
            if (!conn->KeepAlive()) return;
            if (m_control && m_control->IsDraining()) return;   // exit promptly on drain
            if (!conn->Reset()) return;
        }
    }

    DrainRegistration   m_registration;
    ServerParserOptions m_parserOptions;
    ConnectionOpenHandler m_connectionOpen;
    ConnectionCloseHandler m_connectionClose;
    RequestAdmissionHandler m_requestAdmission;
    size_t m_maxRequestHeadBytes;
    io::Descriptor      m_fd;
    io::ShutdownOnKillGuard m_shutdownGuard;
    Cooperator*         m_co;
    RequestHandler      m_handler;
    void*               m_userData;
    time::Interval      m_timeout;
    bool                m_pbufRecv;
    ServerHandle*       m_control;
};

// -------------------------------------------------------------------------------------
// TLS HTTP connection handler
// -------------------------------------------------------------------------------------

struct HttpTlsConnection : Launchable
{
    HttpTlsConnection(Context* ctx, int fd, Cooperator* co,
                      RequestHandler handler, void* userData,
                      io::ssl::Context& sslCtx,
                      time::Interval timeout,
                      ServerHandle* control = nullptr,
                      ServerParserOptions parserOptions = {},
                      ConnectionOpenHandler connectionOpen = nullptr,
                      ConnectionCloseHandler connectionClose = nullptr,
                      RequestAdmissionHandler requestAdmission = nullptr,
                      size_t maxRequestHeadBytes = 16384)
    : Launchable(ctx)
    , m_registration(control)
    , m_parserOptions(parserOptions)
    , m_connectionOpen(connectionOpen)
    , m_connectionClose(connectionClose)
    , m_requestAdmission(requestAdmission)
    , m_maxRequestHeadBytes(maxRequestHeadBytes)
    , m_fd(fd)
    , m_shutdownGuard(ctx, m_fd, SHUT_RD)
    , m_co(co)
    , m_handler(handler)
    , m_userData(userData)
    , m_sslCtx(sslCtx)
    , m_timeout(timeout)
    , m_control(control)
    {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

        // Disable Nagle's algorithm. TLS encrypts each SSL_write as a separate record, so the
        // HTTP response becomes multiple TCP segments. Nagle holds the second segment until
        // the first is ACKed; combined with the client's delayed ACK (~40ms), this creates a
        // catastrophic latency floor per request.
        //
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        ctx->SetName("HttpTlsConnection");
    }

    virtual void Launch() final
    {
        // Register before handshake: a peer that never sends ClientHello is still live.
        m_registration.Begin(GetContext(), &m_fd);
        ConnectionLifetime lifetime{
            GetContext(), m_fd, m_connectionOpen, m_connectionClose, m_userData};
        if (!lifetime.Open()) return;
        char sslBuf[io::ssl::Connection::BUFFER_SIZE];
        io::ssl::Connection sslConn(m_sslCtx, m_fd, sslBuf, sizeof(sslBuf));
        if (sslConn.HandshakeKill() != 0) return;
        if (m_control && m_control->IsDraining()) return;

        using Conn = Connection<TlsTransport>;
        TlsTransport transport(sslConn, m_fd);
        auto conn = GetContext()->Allocate<Conn>(
            Conn::ExtraBytes(), transport, GetContext(), m_co,
            ConnectionBase::DEFAULT_BUFFER_SIZE, ConnectionBase::DEFAULT_SEND_BUFFER_SIZE,
            m_timeout, m_parserOptions, lifetime.state);
        DrainParser parserRegistration(m_registration, conn.get());
        RefusalClose refusal{*conn, m_fd};

        ResponseClosure<Conn> closure{*conn, m_fd, GetContext(), &sslConn, m_timeout};
        ConnectionChildren children{lifetime};
        while (!GetContext()->IsKilled())
        {
            if (m_control && m_control->IsDraining()) conn->ForceClose();
            if (!HandleRequest(*conn, m_handler, m_userData,
                               m_control ? &m_registration.node : nullptr,
                               m_requestAdmission, m_maxRequestHeadBytes)) return;

            if (conn->SendError()) return;
            // A completed close-delimited response cannot be reused. Do not turn
            // later cancellation into a receive failure while draining the request.
            //
            if (conn->CloseDelimitedResponseComplete() &&
                (GetContext()->IsKilled() || GetContext()->RemainingUs() <= 0)) return;

            // Drain the request before judging keep-alive: a handler that never touched
            // the headers hasn't parsed Connection yet, and checking first would let
            // Reset() wipe a close that SkipBody just discovered — a phantom extra
            // iteration against a closed peer.
            //
            if (!conn->SkipBody()) return;
            if (!conn->KeepAlive()) return;
            if (m_control && m_control->IsDraining()) return;
            if (!conn->Reset()) return;
        }
    }

    DrainRegistration   m_registration;
    ServerParserOptions m_parserOptions;
    ConnectionOpenHandler m_connectionOpen;
    ConnectionCloseHandler m_connectionClose;
    RequestAdmissionHandler m_requestAdmission;
    size_t m_maxRequestHeadBytes;
    io::Descriptor      m_fd;
    io::ShutdownOnKillGuard m_shutdownGuard;
    Cooperator*         m_co;
    RequestHandler      m_handler;
    void*               m_userData;
    io::ssl::Context&   m_sslCtx;
    time::Interval      m_timeout;
    ServerHandle*       m_control;
};

// Create, bind, and listen the server socket with checked returns. An assert-only
// version of this block once let a release build miss bind(443) (EACCES without
// CAP_NET_BIND_SERVICE) and then listen() autobound an ephemeral port — a
// healthy-looking server unreachable on its configured port, with no diagnostic.
// Returns the listening fd, or -1 with the failure logged.
//
static int BindListen(ServerConfiguration const& config)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    if (!config.bindAddress || inet_pton(AF_INET, config.bindAddress, &addr.sin_addr) != 1)
    {
        spdlog::error("http server invalid IPv4 bind address: {}",
                      config.bindAddress ? config.bindAddress : "(null)");
        return -1;
    }

    int serverFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (serverFd < 0)
    {
        spdlog::error("http server socket() failed: {}", strerror(errno));
        return -1;
    }

    int on = 1;
    if (setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
        (config.reusePort &&
         setsockopt(serverFd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0))
    {
        spdlog::error("http server setsockopt failed: {}", strerror(errno));
        close(serverFd);
        return -1;
    }

    if (bind(serverFd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) != 0)
    {
        spdlog::error("http server bind(port={}) failed: {}", config.port,
                      strerror(errno));
        close(serverFd);
        return -1;
    }

    if (listen(serverFd, config.backlog) != 0)
    {
        spdlog::error("http server listen(port={}) failed: {}", config.port,
                      strerror(errno));
        close(serverFd);
        return -1;
    }

    return serverFd;
}

// The accept loop, shared by plaintext and TLS servers. One-shot accepts go through
// the kill-aware blocking op; the multishot path arms one SQE for the listener's
// lifetime and parks kill-aware (ArmedAccept::ParkKillAware) — a kill wakes the loop
// directly, and ArmedAccept's teardown drain cancels the armed op ring-side. No
// listener-shutdown guard: a second waiter on the same kill signal would race the
// kill-aware park for a single notify, and a listener shutdown does not reliably
// terminate an armed multishot accept anyway.
//
template<typename LaunchFn>
static void AcceptLoop(Context* ctx, io::Descriptor& desc,
                       ServerConfiguration const& config, LaunchFn&& launch)
{
    auto draining = [&] { return config.control && config.control->IsDraining(); };

    if (config.multishotAccept)
    {
        Coordinator coord;
        io::ArmedAccept armed(ctx, desc, &coord, config.maxPendingAccepts);
        armed.Arm();

        while (!ctx->IsKilled() && !draining())
        {
            int fd = armed.Next();
            if (fd < 0)
            {
                break;
            }
            if (draining()) { ::close(fd); break; }
            launch(fd);
            ctx->Yield();
        }
        return;
    }

    while (!ctx->IsKilled() && !draining())
    {
        int fd = io::AcceptKill(desc);
        if (fd < 0)
        {
            break;
        }
        if (draining()) { ::close(fd); break; }
        launch(fd);
        ctx->Yield();
    }
}

} // end anonymous namespace

bool RunServer(Context* ctx, ServerConfiguration const& config)
{
    if (!config.handler)
    {
        spdlog::error("http RunServer: config.handler is null");
        return false;
    }

    ctx->SetName(config.name);

    int serverFd = BindListen(config);
    if (serverFd < 0)
    {
        return false;
    }

    auto* co = ctx->GetCooperator();
    io::Descriptor desc(serverFd);
    ListenerRegistration listenerRegistration(config.control, &desc);

    AcceptLoop(ctx, desc, config, [&](int fd)
    {
        static constexpr SpawnConfiguration spawn = {.priority = 0, .stackSize = 32768};
        if (!co->Launch<HttpConnection>(spawn, fd, co, config.handler, config.userData,
                                       config.timeout, config.pbufRecv, config.control,
                                       config.parserOptions, config.connectionOpen,
                                       config.connectionClose, config.requestAdmission,
                                       config.maxRequestHeadBytes))
        {
            ::close(fd);
        }
    });
    return true;
}

bool RunTlsServer(Context* ctx, ServerConfiguration const& config, io::ssl::Context& sslCtx)
{
    if (!config.handler)
    {
        spdlog::error("http RunTlsServer: config.handler is null");
        return false;
    }

    ctx->SetName(config.name);

    int serverFd = BindListen(config);
    if (serverFd < 0)
    {
        return false;
    }

    auto* co = ctx->GetCooperator();
    io::Descriptor desc(serverFd);
    ListenerRegistration listenerRegistration(config.control, &desc);

    AcceptLoop(ctx, desc, config, [&](int fd)
    {
        // TLS handshake + HTTP requires more stack for OpenSSL
        //
        static constexpr SpawnConfiguration spawn = {.priority = 0, .stackSize = 65536};
        if (!co->Launch<HttpTlsConnection>(spawn, fd, co, config.handler, config.userData,
                                          sslCtx, config.timeout, config.control,
                                          config.parserOptions, config.connectionOpen,
                                          config.connectionClose, config.requestAdmission,
                                          config.maxRequestHeadBytes))
        {
            ::close(fd);
        }
    });
    return true;
}

} // end namespace coop::http
} // end namespace coop
