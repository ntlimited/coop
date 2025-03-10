#include <cstring>
#include <fcntl.h>
#include <functional>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>

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
#include "coop/time/ticker.h"
#include "coop/tricks.h"

#include "HTTPRequest.hpp"

// Demo program that currently sets up a TCP echo server by:
//
// - creating a coordinator and starting its thread
// - launching a master task that does the actual work
// - creating a Router implementation for handling eventing
// - creating a TCP server that will use the router for dispatching events
// - attaching an echo handler to serve sockets connected through the server
//

struct EchoHandler : coop::network::TCPHandler
{
    using List = ::coop::EmbeddedList<EchoHandler>;

    EchoHandler(coop::Context* ctx, int fd, void*)
    : coop::network::TCPHandler(ctx, fd)
    , m_totalBytes(0)
    {
    }

    // Recv is the code invoked when data arrives on the socket. Returning false
    //
    bool Recv(coop::Context* ctx, void* buffer, const size_t bytes) final
    {
        void* copied = malloc(bytes);
        if (!copied)
        {
            return false;
        }
        memcpy(copied, buffer, bytes);

        // Some profligate task spawning and coordination hijinks
        //
        bool ok = ctx->GetCooperator()->Spawn([=,this](coop::Context* taskCtx)
        {
            taskCtx->SetName("delayed_send");
            auto* myCopied = copied;

            coop::time::Sleeper s(
                taskCtx,
                taskCtx->GetCooperator()->GetTicker(),
                std::chrono::seconds(1));

            s.Submit();
            
            if (s.GetCoordinator() != CoordinateWithKill(taskCtx, s.GetCoordinator()))
            {
                return;
            }


            if (m_coordinator != CoordinateWithKill(taskCtx, &m_coordinator))
            {
                return;
            }

            if (Send(taskCtx, myCopied, bytes))
            {
                m_totalBytes += bytes;
            }
            m_coordinator.Release(taskCtx);
            free(copied);
        });

        if (!ok)
        {
            free(copied);
            return false;
        }
            
        return true;
    }

    coop::Coordinator m_coordinator;
    size_t m_totalBytes;
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
    int serverFd = bind_and_listen(8888);
    // int epollFd = epoll_create(32);

    // assert(epollFd >= 0 && serverFd >= 0);

	auto* co = ctx->GetCooperator();
    auto* ticker = co->Launch<coop::time::Ticker>();
    co->SetTicker(ticker);

    // Handles for the epoll we will use to manage eventing, and the listener which will
    // accept connections on a socket and be alerted via epoll
    //
    coop::Handle routerHandle;
    coop::Handle serverHandle;

    // Begin the epoll controller and its TCP handler loop
    //
    // coop::network::EpollRouter router(epollFd);

    auto* router = co->Launch<coop::network::PollRouter>(&routerHandle);
    auto* server = co->Launch<coop::network::TCPServer<EchoHandler, void*>>(&serverHandle, serverFd, nullptr);
    server->Register(router);

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
            printf("---- Cooperator ----\n");
            printf("Total: %lu\nYielded: %lu\nBlocked: %lu\n",
                statusCtx->GetCooperator()->ContextsCount(),
                statusCtx->GetCooperator()->YieldedCount(),
                statusCtx->GetCooperator()->BlockedCount());
            statusCtx->GetCooperator()->PrintContextTree();
        }
    });

    // Wait for either the router or us to get killed

    //
    coop::CoordinateWithKill(ctx, routerHandle.GetKilledSignal());
}

int main()
{
	coop::Cooperator cooperator;
    coop::Thread mt(&cooperator);

	cooperator.Submit(&SpawningTask);
	return 0;
}
