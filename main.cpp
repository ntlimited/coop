#include <cstring>
#include <fcntl.h>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>

#include "coop/coordinate.h"
#include "coop/coordinator.h"
#include "coop/cooperator.h"
#include "coop/launchable.h"
#include "coop/thread.h"
#include "coop/network/epoll_router.h"
#include "coop/network/tcp_server.h"
#include "coop/time/driver.h"

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
    EchoHandler(int fd)
    : coop::network::TCPHandler(fd)
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

        if (ctx->GetCooperator()->Spawn([=,this](coop::Context* taskCtx)
        {
            m_coordinator.Acquire(taskCtx);
            Send(ctx, copied, bytes);
            m_coordinator.Release(taskCtx);
            free(copied);
        }))
        {
            return true;
        }
        free(copied);
        return false;
    }

    coop::Coordinator m_coordinator;
};

struct EchoHandlerFactory : coop::network::TCPHandlerFactory
{
    coop::network::TCPHandler* Handler(int fd) final
    {
        return new EchoHandler(fd);
    }

    void Delete(coop::network::TCPHandler* h)
    {
        delete h;
    }
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

    // Handles for the epoll we will use to manage eventing, and the listener which will
    // accept connections on a socket and be alerted via epoll
    //
    coop::Handle routerHandle;
    coop::Handle serverHandle;
    coop::Handle timerHandle;

    // Begin the epoll controller and its TCP handler loop
    //
    EchoHandlerFactory factory;
    coop::network::EpollRouter router(epollFd);
    coop::network::TCPServer server(serverFd, &factory);
    server.Register(&router);
    
    co->Launch(router, &routerHandle);
    co->Launch(server, &serverHandle);

    coop::time::Driver timeDriver;
    co->Launch(timeDriver, &timerHandle);

    co->Spawn([&](coop::Context* ctx)
    {
        coop::Coordinator coord;
        coop::time::Handle h(std::chrono::milliseconds(10), &coord);

        while (!ctx->IsKilled())
        {
            printf("Submitting timer to driver\n");
            h.Submit(&timeDriver);
            h.Wait(ctx);
            assert(!coord.IsHeld());
            printf("Timer triggered!\n");
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
