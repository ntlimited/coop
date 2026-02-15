#include "server.h"

#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

#include "coop/cooperator.h"
#include "coop/launchable.h"
#include "coop/io/io.h"
#include "coop/io/read_file.h"

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

// Build a complete HTTP/1.1 response from status code, content type, and body
//
std::string FormatResponse(int code, const char* status, const char* contentType,
                           const std::string& body)
{
    std::string resp;
    resp.reserve(256 + body.size());
    resp += "HTTP/1.1 ";
    resp += std::to_string(code);
    resp += ' ';
    resp += status;
    resp += "\r\nContent-Type: ";
    resp += contentType;
    resp += "\r\nContent-Length: ";
    resp += std::to_string(body.size());
    resp += "\r\nConnection: close\r\n\r\n";
    resp += body;
    return resp;
}

struct HttpConnection : Launchable
{
    HttpConnection(Context* ctx, int fd, Cooperator* co,
                   const Route* routes, int routeCount,
                   const char* const* searchPaths)
    : Launchable(ctx)
    , m_fd(fd)
    , m_co(co)
    , m_routes(routes)
    , m_routeCount(routeCount)
    , m_searchPaths(searchPaths)
    , m_stream(m_fd)
    {
        ctx->SetName("HttpConnection");
    }

    virtual void Launch() final
    {
        char buf[4096];
        int n = m_stream.Recv(buf, sizeof(buf) - 1);
        if (n <= 0)
        {
            return;
        }
        buf[n] = '\0';

        // Find end of request line
        //
        char* lineEnd = strstr(buf, "\r\n");
        if (!lineEnd)
        {
            auto resp = FormatResponse(400, "Bad Request", "text/plain", "Bad Request\n");
            m_stream.SendAll(resp.data(), resp.size());
            return;
        }
        *lineEnd = '\0';

        // Verify GET method
        //
        if (strncmp(buf, "GET ", 4) != 0)
        {
            auto resp = FormatResponse(405, "Method Not Allowed", "text/plain",
                                       "Method Not Allowed\n");
            m_stream.SendAll(resp.data(), resp.size());
            return;
        }

        // Extract path (between "GET " and next space)
        //
        char* pathStart = buf + 4;
        char* pathEnd = strchr(pathStart, ' ');
        if (pathEnd)
        {
            *pathEnd = '\0';
        }

        // Linear scan route match
        //
        for (int i = 0; i < m_routeCount; i++)
        {
            if (strcmp(pathStart, m_routes[i].path) == 0)
            {
                std::string body = m_routes[i].handler(m_co);
                auto resp = FormatResponse(200, "OK", m_routes[i].contentType, body);
                m_stream.SendAll(resp.data(), resp.size());
                return;
            }
        }

        // File serving fallback
        //
        if (m_searchPaths && !HasPathTraversal(pathStart))
        {
            const char* uriPath = pathStart;
            if (strcmp(uriPath, "/") == 0)
            {
                uriPath = "/index.html";
            }

            char filePath[512];
            char fileBuf[65536];

            for (const char* const* sp = m_searchPaths; *sp != nullptr; sp++)
            {
                int len = snprintf(filePath, sizeof(filePath), "%s%s", *sp, uriPath);
                if (len < 0 || static_cast<size_t>(len) >= sizeof(filePath))
                {
                    continue;
                }

                int bytes = io::ReadFile(filePath, fileBuf, sizeof(fileBuf));
                if (bytes >= 0)
                {
                    const char* ct = ContentTypeForExtension(filePath);
                    std::string body(fileBuf, static_cast<size_t>(bytes));
                    auto resp = FormatResponse(200, "OK", ct, body);
                    m_stream.SendAll(resp.data(), resp.size());
                    return;
                }
            }
        }

        auto resp = FormatResponse(404, "Not Found", "text/plain", "Not Found\n");
        m_stream.SendAll(resp.data(), resp.size());
    }

    io::Descriptor      m_fd;
    Cooperator*         m_co;
    const Route*        m_routes;
    int                 m_routeCount;
    const char* const*  m_searchPaths;
    io::PlaintextStream m_stream;
};

} // end anonymous namespace

void RunServer(
    Context* ctx,
    int port,
    const Route* routes,
    int routeCount,
    const char* name /* = "HttpServer" */,
    const char* const* searchPaths /* = nullptr */)
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

        co->Launch<HttpConnection>(fd, co, routes, routeCount, searchPaths);
        ctx->Yield();
    }
}

} // end namespace coop::http
} // end namespace coop
