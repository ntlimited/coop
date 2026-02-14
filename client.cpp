#include <cstring>
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
#include "coop/io/ssl/ssl.h"

void ClientTask(coop::Context* ctx, void* arg)
{
    ctx->SetName("ClientTask");
    bool useSsl = arg != nullptr;

    auto* co = ctx->GetCooperator();
    co->SetTicker(co->Launch<coop::time::Ticker>());
    co->SetUring(co->Launch<coop::io::Uring>(64));

    coop::io::ssl::Context sslCtx(coop::io::ssl::Mode::Client);
    auto* sslCtxPtr = &sslCtx;

    int port = useSsl ? 8889 : 8888;

    for (int i = 0 ; i < 16 ; i++)
    {
        co->Spawn([co, sslCtxPtr, port, useSsl](coop::Context* childCtx)
        {
            int fd = socket(AF_INET , SOCK_STREAM | SOCK_NONBLOCK, 0);
            coop::io::Descriptor remote(fd);
            if (coop::io::Connect(remote, "127.0.0.1", port))
            {
                assert(false);
            }

            if (useSsl)
            {
                coop::io::ssl::Connection conn(*sslCtxPtr, remote);
                if (conn.Handshake() < 0)
                {
                    remote.Close();
                    return;
                }

                char send[] = "hello over tls\n";
                char recv[1024];

                coop::io::ssl::Send(conn, send, strlen(send));
                int n = coop::io::ssl::Recv(conn, recv, sizeof(recv));
                if (n > 0)
                {
                    recv[n] = '\0';
                    printf("TLS echo: %s", recv);
                }

                remote.Close();
            }
            else
            {
                char send[] = "hello over plaintext\n";
                char recv[1024];

                coop::io::Send(remote, send, strlen(send));
                int n = coop::io::Recv(remote, recv, sizeof(recv));
                if (n > 0)
                {
                    recv[n] = '\0';
                    printf("Plaintext echo: %s", recv);
                }

                remote.Close();
            }
        });
    }

    ctx->GetKilledSignal()->Acquire(ctx);
}

int main(int argc, char* argv[])
{
    bool useSsl = false;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--ssl") == 0)
        {
            useSsl = true;
        }
    }

    coop::Cooperator cooperator;
    coop::Thread mt(&cooperator);

    cooperator.Submit(&ClientTask, useSsl ? reinterpret_cast<void*>(1) : nullptr);
    return 0;
}
