#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/multi_coordinator.h"
#include "coop/thread.h"
#include "coop/network/dial.h"
#include "coop/network/epoll_router.h"
#include "coop/network/handle.h"
#include "coop/network/socket.h"
#include "coop/time/ticker.h"

void ClientTask(coop::Context* ctx, void*)
{
    ctx->SetName("ClientTask");

    auto* co = ctx->GetCooperator();

    int epollFd = epoll_create(2);
    assert(epollFd >= 0);
    printf("errno? %d %s\n", errno, strerror(errno));

    int fd = coop::network::Dial("127.0.0.1", 8888);
    printf("errno? %d %s\n", errno, strerror(errno));
    assert(fd >= 0);

    coop::Handle tickerHandle;
    coop::time::Ticker ticker;
    co->SetTicker(&ticker);
    co->Launch(ticker, &tickerHandle);

    coop::Handle routerHandle;
    coop::network::EpollRouter router(epollFd);
    co->Launch(router, &routerHandle);

    coop::Coordinator inCoord(ctx);
    coop::network::Handle inHandle(STDIN_FILENO, ::coop::network::IN, &inCoord);
    inHandle.SetNonBlocking();
    router.Register(&inHandle);

    coop::network::Socket client(fd);
    assert(client.Register(&router));

    client.SendAll(ctx, "hello world", sizeof("hello world"));

    while (!ctx->IsKilled())
    {
        printf("Waiting on inCoord or client coordinator\n");
        switch (coop::CoordinateWith(ctx, &inCoord, client.GetCoordinator()))
        {
            case 0:
                // Data available for inHandle
                //
                {
                    char buffer[128];
                    int ret = read(STDIN_FILENO, &buffer[0], 128);
                    if (ret < 0)
                    {
                        printf("Error reading stdin: %d (%s)\n", errno, strerror(errno));
                        return;
                    }
                    printf("Read from stdin\n");
                    client.SendAll(ctx, &buffer[0], ret);
                }
                // Fallthrough to try and send data off of the list
                //
            case 1:
                {
                    char buffer[128];
                    while (int ret = client.Recv(ctx, &buffer[0], 128, MSG_DONTWAIT))
                    {
                        if (ret < 0)
                        {
                            printf("Error reading from client: %d (%s)\n", errno, strerror(errno));
                            return;
                        }
                        buffer[ret] = 0;
                        printf("Received: %s\n", buffer);
                    }
                }
            default:
                // We were killed
                //
                break;
        }
    }
}


int main()
{
	coop::Cooperator cooperator;
    coop::Thread mt(&cooperator);

	cooperator.Submit(&ClientTask);
    return 0;
}
