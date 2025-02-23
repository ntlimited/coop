#include <cstring>
#include <fcntl.h>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>

#include "coop/coordinate.h"
#include "coop/coordinator.h"
#include "coop/cooperator.h"
#include "coop/embedded_list.h"
#include "coop/launchable.h"
#include "coop/thread.h"
#include "coop/network/epoll_router.h"
#include "coop/network/tcp_server.h"
#include "coop/time/ticker.h"
#include "coop/tricks.h"

// Demo program that currently sets up a TCP echo server by:
//
// - creating a coordinator and starting its thread
// - launching a master task that does the actual work
// - creating a Router implementation for handling eventing
// - creating a TCP server that will use the router for dispatching events
// - attaching an echo handler to serve sockets connected through the server
//

struct EchoHandler : coop::network::TCPHandler, ::coop::EmbeddedListHookups<EchoHandler>
{
    using List = ::coop::EmbeddedList<EchoHandler>;

    EchoHandler(int fd)
    : coop::network::TCPHandler(fd)
    , m_totalBytes(0)
    {
    }

    bool Recv(coop::Context* ctx, void* buffer, const size_t bytes) final
    {
        auto* copied = malloc(bytes);
        if (!copied)
        {
            return false;
        }
        memcpy(copied, buffer, bytes);

        if (ctx->GetCooperator()->Spawn([&](coop::Context* taskCtx)
        {
            m_coordinator.Acquire(taskCtx);
            Send(taskCtx, copied, bytes);
            m_coordinator.Release(taskCtx);
            m_totalBytes += bytes;
            free(copied);
        }))
        {
            return true;
        }
        free(copied);
        return false;
    }

    coop::Coordinator m_coordinator;
    size_t m_totalBytes;
};

struct EchoHandlerFactory : coop::network::TCPHandlerFactory
{
    coop::network::TCPHandler* Handler(int fd) final
    {
        auto* h = new EchoHandler(fd);
        if (!h)
        {
            return nullptr;
        }

        m_handlers.Push(h);
        return h;
    }

    void Delete(coop::network::TCPHandler* h)
    {
        m_handlers.Remove(coop::detail::ascend_cast<EchoHandler>(h));
        delete h;
    }

    void PrintStatus()
    {
        printf("---- EchoHandlerFactory ----\n");
        printf("Currently connected: %lu sessions\n", m_handlers.Size());
        m_handlers.Visit([](EchoHandler* h)
        {
            printf("- %lu total bytes\n", h->m_totalBytes);
            return true;
        });
    }

    EchoHandler::List m_handlers;
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
    int serverFd = bind_and_listen(8888);
    int epollFd = epoll_create(32);

    assert(epollFd >= 0 && serverFd >= 0);

	auto* co = ctx->GetCooperator();

    coop::time::Ticker timeTicker;
    co->SetTicker(&timeTicker);

    // Handles for the epoll we will use to manage eventing, and the listener which will
    // accept connections on a socket and be alerted via epoll
    //
    coop::Handle routerHandle;
    coop::Handle serverHandle;

    // Begin the epoll controller and its TCP handler loop
    //
    EchoHandlerFactory factory;
    coop::network::EpollRouter router(epollFd);
    coop::network::TCPServer server(serverFd, &factory);
    server.Register(&router);

    co->Launch(router, &routerHandle);
    co->Launch(server, &serverHandle);

    co->Spawn([&](coop::Context* ctx)
    {
        coop::Coordinator coord;
        coop::time::Handle h(std::chrono::seconds(1), &coord);

        while (!ctx->IsKilled())
        {
            h.Submit(&timeTicker);
            h.Wait(ctx);
            assert(!coord.IsHeld());
            factory.PrintStatus();
        }
    });

    // Kill epoll and wait for it to die.
    //
    // epollHandle.Kill();
    while (routerHandle)
    {
        ctx->Yield();
    }
}

int main()
{
	coop::Cooperator cooperator;
    coop::Thread mt(&cooperator);

	cooperator.Submit(&SpawningTask);
	return 0;
}
