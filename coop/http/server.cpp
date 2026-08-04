#include "server.h"
#include "connection.h"
#include "transport.h"
#include "tls_transport.h"

#include <cerrno>
#include <optional>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
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

// Serve one request. Returns false when no request could be parsed — clean keep-alive
// EOF or malformed bytes — which must END the connection loop: a clean EOF that keeps
// looping spins hot on instant zero-byte reads (the recv fastpath returns EOF without
// ever parking), monopolizing the cooperator.
//
bool HandleRequest(ConnectionBase& conn, RequestHandler handler, void* userData)
{
    auto* req = conn.GetRequestLine();
    if (!req)
    {
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

    // Everything past parsing is the application's: matching, static serving, 404s. coop just
    // hands over the parsed connection.
    //
    handler(conn, userData);
    return true;
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
                   ServerHandle* control = nullptr)
    : Launchable(ctx)
    , m_fd(fd)
    , m_shutdownGuard(ctx, m_fd)
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
        // Drain mode: detach from the acceptor so stopping it does not cascade-kill this
        // connection, and register with the control so Drain can find and drain it. The
        // node is stack-resident for the connection's lifetime; deregister on exit.
        //
        ServerHandle::ConnNode connNode;
        if (m_control)
        {
            GetContext()->Detach();
            m_control->Register(&connNode, &m_fd);
        }
        DrainDeregister deregister{m_control, &connNode};

        using Conn = Connection<PlaintextTransport>;
        PlaintextTransport transport(m_fd);
        auto conn = GetContext()->Allocate<Conn>(
            Conn::ExtraBytes(), transport, GetContext(), m_co,
            ConnectionBase::DEFAULT_BUFFER_SIZE, ConnectionBase::DEFAULT_SEND_BUFFER_SIZE,
            m_timeout);

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

        while (!GetContext()->IsKilled())
        {
            // Draining: force this response to close the connection, then exit after it.
            //
            if (m_control && m_control->IsDraining())
            {
                conn->ForceClose();
            }
            if (!HandleRequest(*conn, m_handler, m_userData)) return;

            if (conn->SendError()) return;

            // Drain the request before judging keep-alive: a handler that never touched
            // the headers hasn't parsed Connection yet, and checking first would let
            // Reset() wipe a close that SkipBody just discovered — a phantom extra
            // iteration against a closed peer.
            //
            conn->SkipBody();
            if (!conn->KeepAlive()) return;
            if (m_control && m_control->IsDraining()) return;   // exit promptly on drain
            conn->Reset();
        }
    }

    // RAII deregistration from the drain control on any exit path (return, throw).
    //
    struct DrainDeregister
    {
        ServerHandle* control;
        ServerHandle::ConnNode* node;
        ~DrainDeregister() { if (control) control->Deregister(node); }
    };

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
                      time::Interval timeout)
    : Launchable(ctx)
    , m_fd(fd)
    , m_shutdownGuard(ctx, m_fd)
    , m_co(co)
    , m_handler(handler)
    , m_userData(userData)
    , m_sslCtx(sslCtx)
    , m_timeout(timeout)
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
        char sslBuf[io::ssl::Connection::BUFFER_SIZE];
        io::ssl::Connection sslConn(m_sslCtx, m_fd, sslBuf, sizeof(sslBuf));
        if (sslConn.HandshakeKill() != 0) return;

        using Conn = Connection<TlsTransport>;
        TlsTransport transport(sslConn, m_fd);
        auto conn = GetContext()->Allocate<Conn>(
            Conn::ExtraBytes(), transport, GetContext(), m_co,
            ConnectionBase::DEFAULT_BUFFER_SIZE, ConnectionBase::DEFAULT_SEND_BUFFER_SIZE,
            m_timeout);

        while (!GetContext()->IsKilled())
        {
            if (!HandleRequest(*conn, m_handler, m_userData)) return;

            if (conn->SendError()) return;

            // Drain the request before judging keep-alive: a handler that never touched
            // the headers hasn't parsed Connection yet, and checking first would let
            // Reset() wipe a close that SkipBody just discovered — a phantom extra
            // iteration against a closed peer.
            //
            conn->SkipBody();
            if (!conn->KeepAlive()) return;
            conn->Reset();
        }
    }

    io::Descriptor      m_fd;
    io::ShutdownOnKillGuard m_shutdownGuard;
    Cooperator*         m_co;
    RequestHandler      m_handler;
    void*               m_userData;
    io::ssl::Context&   m_sslCtx;
    time::Interval      m_timeout;
};

// Create, bind, and listen the server socket with checked returns. An assert-only
// version of this block once let a release build miss bind(443) (EACCES without
// CAP_NET_BIND_SERVICE) and then listen() autobound an ephemeral port — a
// healthy-looking server unreachable on its configured port, with no diagnostic.
// Returns the listening fd, or -1 with the failure logged.
//
static int BindListen(ServerConfiguration const& config)
{
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

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    addr.sin_addr.s_addr = INADDR_ANY;

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
    if (config.control)
    {
        config.control->SetListener(&desc);
    }

    AcceptLoop(ctx, desc, config, [&](int fd)
    {
        static constexpr SpawnConfiguration spawn = {.priority = 0, .stackSize = 32768};
        co->Launch<HttpConnection>(spawn, fd, co, config.handler, config.userData,
                                   config.timeout, config.pbufRecv, config.control);
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

    AcceptLoop(ctx, desc, config, [&](int fd)
    {
        // TLS handshake + HTTP requires more stack for OpenSSL
        //
        static constexpr SpawnConfiguration spawn = {.priority = 0, .stackSize = 65536};
        co->Launch<HttpTlsConnection>(spawn, fd, co, config.handler, config.userData,
                                      sslCtx, config.timeout);
    });
    return true;
}

} // end namespace coop::http
} // end namespace coop
