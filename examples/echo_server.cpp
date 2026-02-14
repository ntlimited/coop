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

// Demo program that sets up two TCP echo servers:
//
// - A plaintext echo server on port 8888
// - A TLS echo server on port 8889
//
// Both use the same cooperative uring-based I/O. The echo loop itself is transport-agnostic — it
// operates on a Stream&, so plaintext and TLS handlers share the same core logic.
//

void EchoLoop(coop::Context* ctx, coop::io::Stream& stream)
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

struct EchoHandler : coop::Launchable
{
    // Plaintext constructor
    //
    EchoHandler(coop::Context* ctx, int fd)
    : coop::Launchable(ctx)
    , m_fd(fd)
    , m_plaintextStream(std::in_place, m_fd)
    , m_stream(&*m_plaintextStream)
    {
        ctx->SetName("ConnectionHandler");
    }

    // TLS constructor
    //
    EchoHandler(coop::Context* ctx, int fd, coop::io::ssl::Context* sslCtx)
    : coop::Launchable(ctx)
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

    coop::io::Descriptor                            m_fd;
    char                                            m_tlsBuffer[coop::io::ssl::Connection::BUFFER_SIZE];
    std::optional<coop::io::PlaintextStream>        m_plaintextStream;
    std::optional<coop::io::ssl::Connection>        m_conn;
    std::optional<coop::io::ssl::SecureStream>      m_secureStream;
    coop::io::Stream*                               m_stream;
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

void SpawningTask(coop::Context* ctx, void*)
{
    ctx->SetName("SpawningTask");
    spdlog::info("server starting plaintext=8888 tls=8889");

    int serverFd = bind_and_listen(8888);
    int tlsServerFd = bind_and_listen(8889);

    auto* co = ctx->GetCooperator();

    // TLS configuration — shared across all TLS connections
    //
    coop::io::ssl::Context sslCtx(coop::io::ssl::Mode::Server);

    char certBuf[8192];
    int certLen = coop::io::ReadFile("cert.pem", certBuf, sizeof(certBuf));
    assert(certLen > 0);
    assert(sslCtx.LoadCertificate(certBuf, certLen));

    char keyBuf[8192];
    int keyLen = coop::io::ReadFile("key.pem", keyBuf, sizeof(keyBuf));
    assert(keyLen > 0);
    assert(sslCtx.LoadPrivateKey(keyBuf, keyLen));

    coop::Context::Handle serverHandle;

    // Plaintext echo server on port 8888
    //
    co->Spawn([=](coop::Context* serverCtx)
    {
        serverCtx->SetName("PlaintextAcceptor");
        coop::io::Descriptor desc(serverFd);

        while (!serverCtx->IsKilled())
        {
            int fd = coop::io::Accept(desc);
            assert(fd >= 0);

            spdlog::info("plaintext accepted fd={}", fd);
            co->Launch<EchoHandler>(fd);
            serverCtx->Yield();
        }
    }, &serverHandle);

    // TLS echo server on port 8889
    //
    auto* sslCtxPtr = &sslCtx;
    co->Spawn([=](coop::Context* tlsCtx)
    {
        tlsCtx->SetName("TLSAcceptor");
        coop::io::Descriptor desc(tlsServerFd);

        while (!tlsCtx->IsKilled())
        {
            int fd = coop::io::Accept(desc);
            assert(fd >= 0);

            spdlog::info("tls accepted fd={}", fd);
            co->Launch<EchoHandler>(fd, sslCtxPtr);
            tlsCtx->Yield();
        }
    });

    co->Spawn([=](coop::Context* statusCtx)
    {
        statusCtx->SetName("Status context");
        coop::time::Sleeper s(
            statusCtx,
            statusCtx->GetCooperator()->GetTicker(),
            std::chrono::seconds(10));

        while (!statusCtx->IsKilled())
        {
            s.Sleep();
            spdlog::info("cooperator total={} yielded={} blocked={}",
                statusCtx->GetCooperator()->ContextsCount(),
                statusCtx->GetCooperator()->YieldedCount(),
                statusCtx->GetCooperator()->BlockedCount());
            statusCtx->GetCooperator()->PrintContextTree();
        }
    });

    // Wait for either the router or us to get killed
    //
    ctx->GetKilledSignal()->Wait(ctx);
}

int main()
{
    coop::Cooperator cooperator;
    coop::Thread mt(&cooperator);

    cooperator.Submit(&SpawningTask);
    return 0;
}
