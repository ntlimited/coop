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
#include "coop/io/io.h"
#include "coop/io/uring.h"

#include "HTTPRequest.hpp"

// Demo program that currently sets up a TCP echo server by:
//
// - creating a coordinator and starting its thread
// - launching a master task that does the actual work
// - creating a Router implementation for handling eventing
// - creating a TCP server that will use the router for dispatching events
// - attaching an echo handler to serve sockets connected through the server
//

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

	auto* co = ctx->GetCooperator();
    auto* ticker = co->Launch<coop::time::Ticker>();
    co->SetTicker(ticker);
    auto* uring = co->Launch<coop::io::Uring>(64);
    co->SetUring(uring);

    coop::Context::Handle serverHandle;


    co->Spawn([=](coop::Context* serverCtx)
    {
        serverCtx->SetName("UringHandler");
        coop::io::Descriptor desc(serverFd, uring);

        while (!serverCtx->IsKilled())
        {
            int fd = coop::io::Accept(desc);
            assert(fd >= 0);

            co->Spawn([=](coop::Context* clientCtx)
            {
                coop::io::Descriptor client(fd);

                char buffer[1025];

                while (!clientCtx->IsKilled())
                {
                    int ret = coop::io::Recv(client, &buffer[0], 1024);
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
                    buffer[ret++] = '\n';

                    int at = 0;
                    while (ret)
                    {
                        int sent = coop::io::Send(client, &buffer[at], ret);
                        if (sent == 0)
                        {
                            close(fd);
                            return;
                        }

                        assert(sent > 0);
                        at += sent;
                        ret -= sent;
                    }
                }

                close(fd);
            });

            serverCtx->Yield();
        }
    }, &serverHandle);

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
    ctx->GetKilledSignal()->Acquire(ctx);
}

int main()
{
	coop::Cooperator cooperator;
    coop::Thread mt(&cooperator);

	cooperator.Submit(&SpawningTask);
	return 0;
}
