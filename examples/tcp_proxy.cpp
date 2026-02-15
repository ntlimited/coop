#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

#include <spdlog/spdlog.h>

#include "coop/cooperator.h"
#include "coop/launchable.h"
#include "coop/thread.h"
#include "coop/io/io.h"
#include "coop/io/shutdown.h"
#include "coop/shutdown.h"

// TCP proxy that accepts client connections and relays bytes bidirectionally to an upstream server.
// Exercises the coop IO and connection lifecycle primitives under realistic conditions.
//
// Usage: tcp_proxy <listen-port> <upstream-ip> <upstream-port>
//

static constexpr size_t BUFFER_SIZE = 8192;

struct ProxyHandler : coop::Launchable
{
    ProxyHandler(coop::Context* ctx, int clientFd, const char* upstreamIp, int upstreamPort)
    : coop::Launchable(ctx)
    , m_client(clientFd)
    , m_upstreamIp(upstreamIp)
    , m_upstreamPort(upstreamPort)
    {
        ctx->SetName("ProxyHandler");
    }

    virtual void Launch() final
    {
        // Connect to upstream
        //
        int upstreamFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (upstreamFd < 0)
        {
            spdlog::warn("proxy: socket() failed fd={}", m_client.m_fd);
            return;
        }

        coop::io::Descriptor upstream(upstreamFd);
        int ret = coop::io::Connect(upstream, m_upstreamIp, m_upstreamPort);
        if (ret < 0)
        {
            spdlog::warn("proxy: connect failed fd={} err={}", m_client.m_fd, ret);
            return;
        }

        spdlog::info("proxy: connected fd={} -> upstream fd={}", m_client.m_fd, upstream.m_fd);

        // Flag set by the child relay when it exits, so the parent knows to half-close
        //
        bool childDone = false;

        auto* ctx = GetContext();
        auto* co = ctx->GetCooperator();

        coop::Context::Handle childHandle;

        // Child context: relay client -> upstream
        //
        auto* upstreamPtr = &upstream;
        auto* clientPtr = &m_client;
        auto* childDonePtr = &childDone;

        co->Spawn([=](coop::Context* childCtx)
        {
            childCtx->SetName("ClientToUpstream");
            char buf[BUFFER_SIZE];

            while (!childCtx->IsKilled())
            {
                int n = coop::io::Recv(*clientPtr, buf, sizeof(buf));
                if (n <= 0)
                {
                    break;
                }

                int sent = coop::io::SendAll(*upstreamPtr, buf, n);
                if (sent <= 0)
                {
                    break;
                }
            }

            *childDonePtr = true;
        }, &childHandle);

        // Parent context: relay upstream -> client
        //
        char buf[BUFFER_SIZE];
        while (!ctx->IsKilled())
        {
            int n = coop::io::Recv(upstream, buf, sizeof(buf));
            if (n <= 0)
            {
                break;
            }

            int sent = coop::io::SendAll(m_client, buf, n);
            if (sent <= 0)
            {
                break;
            }

            // If the child exited (client closed its write side), propagate the half-close
            // to upstream so it knows no more data is coming, then keep draining.
            //
            if (childDone)
            {
                coop::io::Shutdown(upstream, SHUT_WR);
                childDone = false;
            }
        }

        // Parent is done — kill the child if it's still running. RAII closes both descriptors.
        //
        if (childHandle)
        {
            childHandle.Kill();
        }
    }

    coop::io::Descriptor    m_client;
    const char*             m_upstreamIp;
    int                     m_upstreamPort;
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

struct ProxyConfig
{
    int listenPort;
    const char* upstreamIp;
    int upstreamPort;
};

void SpawningTask(coop::Context* ctx, void* arg)
{
    ctx->SetName("SpawningTask");
    auto* config = static_cast<ProxyConfig*>(arg);

    spdlog::info("proxy starting listen={} upstream={}:{}",
        config->listenPort, config->upstreamIp, config->upstreamPort);

    int serverFd = bind_and_listen(config->listenPort);
    auto* co = ctx->GetCooperator();

    auto* upstreamIp = config->upstreamIp;
    auto upstreamPort = config->upstreamPort;

    co->Spawn([=](coop::Context* acceptCtx)
    {
        acceptCtx->SetName("Acceptor");
        coop::io::Descriptor desc(serverFd);

        while (!acceptCtx->IsKilled())
        {
            int fd = coop::io::Accept(desc);
            if (fd < 0)
            {
                break;
            }

            spdlog::info("proxy: accepted fd={}", fd);
            co->Launch<ProxyHandler>(fd, upstreamIp, upstreamPort);
            acceptCtx->Yield();
        }
    });

    while (!co->IsShuttingDown())
    {
        ctx->Yield(true);
    }
    spdlog::info("shutting down...");
}

int main(int argc, char* argv[])
{
    if (argc != 4)
    {
        spdlog::error("usage: tcp_proxy <listen-port> <upstream-ip> <upstream-port>");
        return 1;
    }

    ProxyConfig config;
    config.listenPort = atoi(argv[1]);
    config.upstreamIp = argv[2];
    config.upstreamPort = atoi(argv[3]);

    if (config.listenPort <= 0 || config.upstreamPort <= 0)
    {
        spdlog::error("invalid port");
        return 1;
    }

    coop::InstallShutdownHandler();
    coop::Cooperator cooperator;
    coop::Thread mt(&cooperator);

    cooperator.Submit(&SpawningTask, &config);
    return 0;
}
