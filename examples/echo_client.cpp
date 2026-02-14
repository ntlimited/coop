#include <cstring>
#include <sys/socket.h>

#include <spdlog/spdlog.h>

#include "coop/cooperator.h"
#include "coop/thread.h"
#include "coop/io/io.h"
#include "coop/io/ssl/ssl.h"

void ClientTask(coop::Context* ctx, void* arg)
{
    ctx->SetName("ClientTask");
    bool useSsl = arg != nullptr;
    spdlog::info("client starting mode={} port={}",
        useSsl ? "ssl" : "plaintext", useSsl ? 8889 : 8888);

    auto* co = ctx->GetCooperator();

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
                spdlog::warn("client connect failed fd={}", fd);
                remote.Close();
                return;
            }

            if (useSsl)
            {
                char tlsBuffer[coop::io::ssl::Connection::BUFFER_SIZE];
                coop::io::ssl::Connection conn(*sslCtxPtr, remote, tlsBuffer, sizeof(tlsBuffer));
                if (conn.Handshake() < 0)
                {
                    spdlog::warn("client tls handshake failed fd={}", fd);
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
                    spdlog::info("tls echo: {}", recv);
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
                    spdlog::info("plaintext echo: {}", recv);
                }

                remote.Close();
            }
        });
    }

    ctx->GetKilledSignal()->Wait(ctx);
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
