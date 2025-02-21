#include <fcntl.h>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>

#include "coop/coordinator.h"
#include "coop/cooperator.h"
#include "coop/launchable.h"
#include "coop/thread.h"
#include "coop/network/epoll_router.h"
#include "coop/network/tcp_server.h"

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
        return Send(ctx, buffer, bytes);
    }
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
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    int ret = bind(serverFd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
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

    // Begin the epoll controller and its TCP handler loop
    //
    EchoHandlerFactory factory;
    coop::network::EpollRouter router(epollFd);
    coop::network::TCPServer server(serverFd, &factory);
    server.Register(&router);
    
    co->Launch(router, &routerHandle);
    co->Launch(server, &serverHandle);

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
