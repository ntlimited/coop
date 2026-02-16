#include <cstring>
#include <optional>
#include <netinet/in.h>
#include <sys/socket.h>

#include <spdlog/spdlog.h>

#include "coop/cooperator.h"
#include "coop/launchable.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"
#include "coop/io/io.h"
#include "coop/io/ssl/ssl.h"
#include "coop/coordinate_with.h"
#include "coop/shutdown.h"
#include "coop/http/status.h"

using namespace coop;

// Demo program that sets up two TCP echo servers:
//
// - A plaintext echo server on port 8888
// - A TLS echo server on port 8889
//
// A single acceptor context multiplexes both listening sockets using CoordinateWith to
// demonstrate composing multiple async IO operations. The echo loop itself is transport-agnostic —
// it operates on a Stream&, so plaintext and TLS handlers share the same core logic.
//

void EchoLoop(Context* ctx, io::Stream& stream)
{
    char buffer[4096];

    while (!ctx->IsKilled())
    {
        int ret = stream.Recv(&buffer[0], 4096);
        if (ret <= 0)
        {
            break;
        }

        int sent = stream.SendAll(&buffer[0], ret);
        if (sent <= 0)
        {
            break;
        }
    }
}

struct EchoHandler : Launchable
{
    // Plaintext constructor
    //
    EchoHandler(Context* ctx, int fd)
    : Launchable(ctx)
    , m_fd(fd)
    , m_plaintextStream(std::in_place, m_fd)
    , m_stream(&*m_plaintextStream)
    {
        ctx->SetName("ConnectionHandler");
    }

    // TLS constructor
    //
    EchoHandler(Context* ctx, int fd, io::ssl::Context* sslCtx)
    : Launchable(ctx)
    , m_fd(fd)
    , m_conn(std::in_place, *sslCtx, m_fd, m_tlsBuffer, sizeof(m_tlsBuffer))
    , m_secureStream(std::in_place, *m_conn)
    , m_stream(&*m_secureStream)
    {
        ctx->SetName("TLSConnectionHandler");
    }

    virtual void Launch() final
    {
        if (m_conn)
        {
            if (m_conn->Handshake() < 0)
            {
                spdlog::warn("tls handshake failed fd={}", m_fd.m_fd);
                return;
            }
            spdlog::info("tls handshake ok fd={}", m_fd.m_fd);
        }
        EchoLoop(GetContext(), *m_stream);
    }

    io::Descriptor                       m_fd;
    char                                 m_tlsBuffer[io::ssl::Connection::BUFFER_SIZE];
    std::optional<io::PlaintextStream>   m_plaintextStream;
    std::optional<io::ssl::Connection>   m_conn;
    std::optional<io::ssl::SecureStream> m_secureStream;
    io::Stream*                          m_stream;
};

int bind_and_listen(int port)
{
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

    return serverFd;
}

void SpawningTask(Context* ctx, void*)
{
    spdlog::info("server starting plaintext=8888 tls=8889");
    
    {
        // Search paths for static files — covers running from project root or from the bin directory
        //
        static const char* staticPaths[] = {
            "static",
            "build/debug/bin/static",
            "build/release/bin/static",
            nullptr,
        };
        http::SpawnStatusServer(GetCooperator(), 8080, staticPaths);
    }

    int serverFd = bind_and_listen(8888);
    int tlsServerFd = bind_and_listen(8889);

    // TLS configuration — shared across all TLS connections
    //
    io::ssl::Context sslCtx(io::ssl::Mode::Server);

    char certBuf[8192];
    int certLen = io::ReadFile("cert.pem", certBuf, sizeof(certBuf));
    assert(certLen > 0);
    assert(sslCtx.LoadCertificate(certBuf, certLen));

    char keyBuf[8192];
    int keyLen = io::ReadFile("key.pem", keyBuf, sizeof(keyBuf));
    assert(keyLen > 0);
    assert(sslCtx.LoadPrivateKey(keyBuf, keyLen));

    // EchoHandler embeds a 16KB TLS buffer, so it needs a larger stack than the default
    //
    static constexpr SpawnConfiguration handlerConfig = {
        .priority = 0,
        .stackSize = 65536,
    };

    ctx->SetName("Acceptor");
    io::Descriptor plaintextDesc(serverFd);
    io::Descriptor tlsDesc(tlsServerFd);

    Coordinator plaintextCoord, tlsCoord;
    io::Handle plaintextHandle(ctx, plaintextDesc, &plaintextCoord);
    io::Handle tlsHandle(ctx, tlsDesc, &tlsCoord);

    io::Accept(plaintextHandle);
    io::Accept(tlsHandle);

    while (true)
    {
        auto result = CoordinateWith(ctx, &plaintextCoord, &tlsCoord);
        if (result.Killed()) break;

        if (result == plaintextCoord)
        {
            int fd = plaintextHandle.Result();
            plaintextCoord.Release(ctx, false);
            spdlog::info("plaintext accepted fd={}", fd);
            Launch<EchoHandler>(handlerConfig, fd);
            io::Accept(plaintextHandle);
        }
        else if (result == tlsCoord)
        {
            int fd = tlsHandle.Result();
            tlsCoord.Release(ctx, false);
            spdlog::info("tls accepted fd={}", fd);
            Launch<EchoHandler>(handlerConfig, fd, &sslCtx);
            io::Accept(tlsHandle);
        }
    }

    spdlog::info("shutting down...");
}

int main()
{
    InstallShutdownHandler();
    Cooperator cooperator;
    Thread mt(&cooperator);

    cooperator.Submit(&SpawningTask, nullptr, {.priority = 0, .stackSize = 65536});
    return 0;
}
