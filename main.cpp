#include <fcntl.h>
#include <vector>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "coordinator.h"
#include "iomgr.h"
#include "launchable.h"
#include "manager_thread.h"
#include "epoll.h"
#include "tcp_server.h"

struct EchoHandler : TCPHandler
{
    EchoHandler(int fd)
    : TCPHandler(fd)
    {
    }

    bool Recv(ExecutionContext* ctx, void* buffer, const size_t bytes) final
    {
        return Send(ctx, buffer, bytes);
    }
};

struct EchoHandlerFactory : TCPHandlerFactory
{
    TCPHandler* Handler(int fd) final
    {
        return new EchoHandler(fd);
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

void SpawningTask(ExecutionContext* ctx, void*)
{
	auto* mgr = ctx->GetManager();
    
    // Handles for the epoll we will use to manage eventing, and the listener which will
    // accept connections on a socket and be alerted via epoll
    //
    ExecutionHandle epollHandle, listenerHandle;

    int serverFd = bind_and_listen(8888);
    int epollFd = epoll_create(32);

    assert(epollFd >= 0 && serverFd >= 0);

    // Begin the epoll controller and its TCP handler loop
    //
    EchoHandlerFactory factory;
    TCPServer server(serverFd, &factory);

    EpollController l(epollFd);
    assert(server.Register(&l));
    
    mgr->Launch(l, &epollHandle);
    mgr->Launch(server, &listenerHandle);


    // Kill epoll and wait for it to die.
    //
    // epollHandle.Kill();
    while (epollHandle)
    {
        ctx->Yield();
    }
}

int main()
{
	Manager manager;
    ManagerThread mt(&manager);

	manager.Submit(&SpawningTask);
	return 0;
}
