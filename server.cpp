#include <cstring>
#include <fcntl.h>
#include <functional>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>

#include <spdlog/spdlog.h>

#include "coop/coordinator.h"
#include "coop/cooperator.h"
#include "coop/multi_coordinator.h"
#include "coop/embedded_list.h"
#include "coop/launchable.h"
#include "coop/thread.h"
#include "coop/network/epoll_router.h"
#include "coop/network/poll_router.h"
#include "coop/network/tcp_handler.h"
#include "coop/network/tcp_server.h"
#include "coop/time/sleep.h"
#include "coop/tricks.h"
#include "coop/io/io.h"
#include "coop/io/ssl/ssl.h"

#include "HTTPRequest.hpp"

// Demo program that sets up two TCP echo servers:
//
// - A plaintext echo server on port 8888
// - A TLS echo server on port 8889
//
// Both use the same cooperative uring-based I/O. The TLS version demonstrates coop::io::ssl usage
// — note how similar TLSClientHandler is to the plaintext ClientHandler.
//

struct ClientHandler : coop::Launchable
{
    ClientHandler(coop::Context* ctx, int fd)
    : coop::Launchable(ctx)
    , m_fd(fd)
    {
        ctx->SetName("ConnectionHandler");
    }

    virtual void Launch() final
    {
        char buffer[4096];

        while (!GetContext()->IsKilled())
        {
            int ret = coop::io::Recv(m_fd, &buffer[0], 4096);
            if (!ret)
            {
                // Short read, disconnected
                //
                break;
            }

            if (ret < 0)
            {
                assert(false);
                break;
            }

            int at = 0;
            while (ret)
            {
                int sent = coop::io::Send(m_fd, &buffer[at], ret);
                if (sent == 0)
                {
                    m_fd.Close();
                    return;
                }

                assert(sent > 0);
                at += sent;
                ret -= sent;
            }
        }
        m_fd.Close();
    }
    
    coop::io::Descriptor m_fd;
};

struct TLSClientHandler : coop::Launchable
{
    TLSClientHandler(coop::Context* ctx, int fd, coop::io::ssl::Context* sslCtx)
    : coop::Launchable(ctx)
    , m_fd(fd)
    , m_conn(*sslCtx, m_fd, m_tlsBuffer, sizeof(m_tlsBuffer))
    {
        ctx->SetName("TLSConnectionHandler");
    }

    virtual void Launch() final
    {
        if (m_conn.Handshake() < 0)
        {
            spdlog::warn("tls handshake failed fd={}", m_fd.m_fd);
            m_fd.Close();
            return;
        }
        spdlog::info("tls handshake ok fd={}", m_fd.m_fd);

        char buffer[4096];

        while (!GetContext()->IsKilled())
        {
            int ret = coop::io::ssl::Recv(m_conn, &buffer[0], 4096);
            if (!ret)
            {
                break;
            }

            if (ret < 0)
            {
                break;
            }

            spdlog::debug("tls echo fd={} bytes={}", m_fd.m_fd, ret);
            int at = 0;
            while (ret)
            {
                int sent = coop::io::ssl::Send(m_conn, &buffer[at], ret);
                if (sent <= 0)
                {
                    m_fd.Close();
                    return;
                }

                at += sent;
                ret -= sent;
            }
        }
        m_fd.Close();
    }

    coop::io::Descriptor       m_fd;
    char                       m_tlsBuffer[coop::io::ssl::Connection::BUFFER_SIZE];
    coop::io::ssl::Connection  m_conn;
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
            co->Launch<ClientHandler>(fd);
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
            co->Launch<TLSClientHandler>(fd, sslCtxPtr);
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
