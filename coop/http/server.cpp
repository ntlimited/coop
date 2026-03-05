#include "server.h"
#include "connection.h"
#include "transport.h"
#include "tls_transport.h"

#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

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

// Try to serve a static file matching the requested path from the search paths.
// Returns true if a file was found and served.
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

void HandleRequest(ConnectionBase& conn, const Route* routes, int routeCount,
                   const char* const* searchPaths)
{
    auto* req = conn.GetRequestLine();
    if (!req)
    {
        if (!conn.SendError())
        {
            conn.Send(400, "text/plain", "Bad Request\n");
        }
        return;
    }

    for (int i = 0; i < routeCount; i++)
    {
        if (req->path == routes[i].path)
        {
            routes[i].handler(conn);
            return;
        }
    }

    if (searchPaths && ServeFile(conn, req->path, searchPaths))
    {
        return;
    }

    conn.Send(404, "text/plain", "Not Found\n");
}

// -------------------------------------------------------------------------------------
// Plaintext HTTP connection handler
// -------------------------------------------------------------------------------------

struct HttpConnection : Launchable
{
    HttpConnection(Context* ctx, int fd, Cooperator* co,
                   const Route* routes, int routeCount,
                   const char* const* searchPaths,
                   time::Interval timeout)
    : Launchable(ctx)
    , m_fd(fd)
    , m_co(co)
    , m_routes(routes)
    , m_routeCount(routeCount)
    , m_searchPaths(searchPaths)
    , m_timeout(timeout)
    {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        ctx->SetName("HttpConnection");
    }

    virtual void Launch() final
    {
        PlaintextTransport transport(m_fd);
        auto conn = GetContext()->Allocate<Connection<PlaintextTransport>>(
            ConnectionBase::DEFAULT_BUFFER_SIZE,
            transport, GetContext(), m_co,
            ConnectionBase::DEFAULT_BUFFER_SIZE, m_timeout);

        while (!GetContext()->IsKilled())
        {
            HandleRequest(*conn, m_routes, m_routeCount, m_searchPaths);

            if (conn->SendError() || !conn->KeepAlive()) return;

            conn->SkipBody();
            conn->Reset();
        }
    }

    io::Descriptor      m_fd;
    Cooperator*         m_co;
    const Route*        m_routes;
    int                 m_routeCount;
    const char* const*  m_searchPaths;
    time::Interval      m_timeout;
};

// -------------------------------------------------------------------------------------
// TLS HTTP connection handler
// -------------------------------------------------------------------------------------

struct HttpTlsConnection : Launchable
{
    HttpTlsConnection(Context* ctx, int fd, Cooperator* co,
                      const Route* routes, int routeCount,
                      io::ssl::Context& sslCtx,
                      const char* const* searchPaths,
                      time::Interval timeout)
    : Launchable(ctx)
    , m_fd(fd)
    , m_co(co)
    , m_routes(routes)
    , m_routeCount(routeCount)
    , m_sslCtx(sslCtx)
    , m_searchPaths(searchPaths)
    , m_timeout(timeout)
    {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        ctx->SetName("HttpTlsConnection");
    }

    virtual void Launch() final
    {
        io::ssl::Connection sslConn(m_sslCtx, m_fd, io::ssl::SocketBio{});
        if (sslConn.Handshake() != 0) return;

        TlsTransport transport(sslConn, m_fd);
        auto conn = GetContext()->Allocate<Connection<TlsTransport>>(
            ConnectionBase::DEFAULT_BUFFER_SIZE,
            transport, GetContext(), m_co,
            ConnectionBase::DEFAULT_BUFFER_SIZE, m_timeout);

        while (!GetContext()->IsKilled())
        {
            HandleRequest(*conn, m_routes, m_routeCount, m_searchPaths);

            if (conn->SendError() || !conn->KeepAlive()) return;

            conn->SkipBody();
            conn->Reset();
        }
    }

    io::Descriptor      m_fd;
    Cooperator*         m_co;
    const Route*        m_routes;
    int                 m_routeCount;
    io::ssl::Context&   m_sslCtx;
    const char* const*  m_searchPaths;
    time::Interval      m_timeout;
};

} // end anonymous namespace

void RunServer(
    Context* ctx,
    int port,
    const Route* routes,
    int routeCount,
    const char* name /* = "HttpServer" */,
    const char* const* searchPaths /* = nullptr */,
    time::Interval timeout /* = std::chrono::seconds(30) */)
{
    ctx->SetName(name);

    int serverFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert(serverFd > 0);

    int on = 1;
    int ret = setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    assert(ret == 0);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    ret = bind(serverFd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
    assert(ret == 0);

    ret = listen(serverFd, 32);
    assert(ret == 0);

    auto* co = ctx->GetCooperator();
    io::Descriptor desc(serverFd);

    while (!ctx->IsKilled())
    {
        int fd = io::Accept(desc);
        if (fd < 0)
        {
            break;
        }

        static constexpr SpawnConfiguration config = {.priority = 0, .stackSize = 32768};
        co->Launch<HttpConnection>(config, fd, co, routes, routeCount, searchPaths, timeout);
        ctx->Yield();
    }
}

void RunTlsServer(
    Context* ctx,
    int port,
    const Route* routes,
    int routeCount,
    io::ssl::Context& sslCtx,
    const char* name /* = "HttpsServer" */,
    const char* const* searchPaths /* = nullptr */,
    time::Interval timeout /* = std::chrono::seconds(30) */)
{
    ctx->SetName(name);

    int serverFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert(serverFd > 0);

    int on = 1;
    int ret = setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    assert(ret == 0);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    ret = bind(serverFd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
    assert(ret == 0);

    ret = listen(serverFd, 32);
    assert(ret == 0);

    auto* co = ctx->GetCooperator();
    io::Descriptor desc(serverFd);

    while (!ctx->IsKilled())
    {
        int fd = io::Accept(desc);
        if (fd < 0)
        {
            break;
        }

        // TLS handshake + HTTP requires more stack for OpenSSL
        //
        static constexpr SpawnConfiguration config = {.priority = 0, .stackSize = 65536};
        co->Launch<HttpTlsConnection>(config, fd, co, routes, routeCount,
                                      sslCtx, searchPaths, timeout);
        ctx->Yield();
    }
}

} // end namespace coop::http
} // end namespace coop
