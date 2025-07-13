#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/embedded_list.h"
#include "coop/multi_coordinator.h"
#include "coop/thread.h"
#include "coop/time/ticker.h"
#include "coop/io/io.h"
#include "coop/io/handle.h"
#include "coop/io/uring.h"

void ClientTask(coop::Context* ctx, void*)
{
    ctx->SetName("ClientTask");

    auto* co = ctx->GetCooperator();
    co->SetTicker(co->Launch<coop::time::Ticker>());
    co->SetUring(co->Launch<coop::io::Uring>(64));

    // Set up 16 children that will do 
    for (int i = 0 ; i < 16 ; i++)
    {
        co->Spawn([co](coop::Context* childCtx)
        {
            int fd = socket(AF_INET , SOCK_STREAM | SOCK_NONBLOCK, 0);
            coop::io::Descriptor remote(fd);
            if (coop::io::Connect(remote, "192.168.0.136", 8888))
            {
                assert(false);
            }

            coop::Coordinator   sc, rc;
            coop::io::Handle    sh(childCtx, remote, &sc);
            coop::io::Handle    rh(childCtx, remote, &rc);

            char buff1[1024], buff2[1024];
            char *rb = &buff1[0], *sb = &buff2[0];

            while (!childCtx->IsKilled())
            {
                
            }
        });
    }
}


int main()
{
    coop::Cooperator cooperator;
    coop::Thread mt(&cooperator);

    cooperator.Submit(&ClientTask);
    return 0;
}
