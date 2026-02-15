#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

#include <spdlog/spdlog.h>

#include "coop/cooperator.h"
#include "coop/channel.h"
#include "coop/launchable.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"
#include "coop/io/io.h"
#include "coop/shutdown.h"

// Chat server example
//
// Demonstrates channels and networking working together. Each client gets a reader (the Launch()
// method) and a writer (spawned child). A central broadcaster fans messages out to all connected
// clients via per-client outbound channels.
//
// Architecture:
//
//   Client A Reader --Send--> Broadcast  --> Broadcaster --TrySend--> Outbound A --> Writer A
//   Client B Reader --Send--> Channel    -->             --TrySend--> Outbound B --> Writer B
//
// TrySend is non-blocking: a slow client doesn't stall the broadcaster or other clients.
//

struct Message
{
    char    data[256];
    int32_t len;
    int32_t senderFd;
};

static constexpr int32_t MAX_CLIENTS = 32;

// 17 slots = 16 usable (ring buffer sentinel)
//
static constexpr size_t BROADCAST_BUFFER_SLOTS = 17;

// 9 slots = 8 usable
//
static constexpr size_t OUTBOUND_BUFFER_SLOTS = 9;

struct ClientSlot
{
    bool                        active;
    int32_t                     fd;
    coop::SendChannel<Message>* outbound;
    coop::Context::Handle       writerHandle;
};

struct ClientRegistry
{
    ClientSlot clients[MAX_CLIENTS];

    ClientRegistry()
    {
        memset(clients, 0, sizeof(clients));
    }

    int32_t Register(int32_t fd, coop::SendChannel<Message>* outbound)
    {
        for (int32_t i = 0; i < MAX_CLIENTS; i++)
        {
            if (!clients[i].active)
            {
                clients[i].active = true;
                clients[i].fd = fd;
                clients[i].outbound = outbound;
                return i;
            }
        }
        return -1;
    }

    void Unregister(int32_t slot)
    {
        clients[slot].active = false;
        clients[slot].outbound = nullptr;
    }
};

struct ChatHandler : coop::Launchable
{
    ChatHandler(
        coop::Context* ctx,
        int fd,
        coop::SendChannel<Message>* broadcast,
        ClientRegistry* registry)
    : coop::Launchable(ctx)
    , m_fd(fd)
    , m_broadcast(broadcast)
    , m_registry(registry)
    , m_outbound(ctx, m_outboundBuffer, OUTBOUND_BUFFER_SLOTS)
    , m_stream(m_fd)
    {
        ctx->SetName("ChatClient");
    }

    virtual void Launch() final
    {
        auto* ctx = GetContext();
        auto* co = ctx->GetCooperator();
        int32_t rawFd = m_fd.m_fd;

        // Register with the client registry so the broadcaster can find us
        //
        int32_t slot = m_registry->Register(rawFd, &m_outbound);
        if (slot < 0)
        {
            spdlog::warn("chat: too many clients, rejecting fd={}", rawFd);
            // Note that returning here means that we auto-close the fd
            //
            return;
        }

        spdlog::info("chat: connected fd={} slot={}", rawFd, slot);

        // Spawn the writer context as a child. It reads from our outbound channel and writes
        // to the socket.
        //
        coop::RecvChannel<Message>* outbound = &m_outbound;
        auto* stream = &m_stream;
        co->Spawn([outbound, stream, rawFd](coop::Context* writerCtx)
        {
            writerCtx->SetName("ChatWriter");
            Message msg;

            while (outbound->Recv(writerCtx, msg))
            {
                int sent = stream->SendAll(msg.data, msg.len);
                if (sent <= 0)
                {
                    break;
                }
            }
        }, &m_registry->clients[slot].writerHandle);

        // Reader loop: recv from socket, prefix with [fd=N], send to broadcast channel
        //
        char recvBuf[220];

        while (!ctx->IsKilled())
        {
            int n = m_stream.Recv(recvBuf, sizeof(recvBuf));
            if (n <= 0)
            {
                break;
            }

            Message msg;
            msg.len = snprintf(msg.data, sizeof(msg.data), "[fd=%d] %.*s", rawFd, n, recvBuf);
            msg.senderFd = rawFd;

            if (!m_broadcast->Send(ctx, msg))
            {
                break;
            }
        }

        // Clean up: unregister before our stack goes away, then shut down the outbound channel
        // (wakes the writer if it's blocked on Recv) and kill the writer.
        //
        spdlog::info("chat: disconnected fd={} slot={}", rawFd, slot);
        m_registry->Unregister(slot);
        m_outbound.Shutdown(ctx);

        if (m_registry->clients[slot].writerHandle)
        {
            m_registry->clients[slot].writerHandle.Kill();
        }
    }

    coop::io::Descriptor        m_fd;
    coop::SendChannel<Message>* m_broadcast;
    ClientRegistry*             m_registry;
    Message                     m_outboundBuffer[OUTBOUND_BUFFER_SLOTS];
    coop::Channel<Message>      m_outbound;
    coop::io::PlaintextStream   m_stream;
};

int BindAndListen(int port)
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
    spdlog::info("chat server starting on port 9000");

    int serverFd = BindAndListen(9000);
    auto* co = ctx->GetCooperator();

    // Broadcast channel and client registry live on this context's stack
    //
    Message broadcastBuffer[BROADCAST_BUFFER_SLOTS];
    coop::Channel<Message> broadcast(ctx, broadcastBuffer, BROADCAST_BUFFER_SLOTS);
    ClientRegistry registry;

    // Acceptor: binds and listens, launches a ChatHandler per connection
    //
    co->Spawn([co, serverFd, &broadcast, &registry](coop::Context* acceptCtx)
    {
        acceptCtx->SetName("Acceptor");
        coop::io::Descriptor desc(serverFd);

        while (!acceptCtx->IsKilled())
        {
            int fd = coop::io::Accept(desc);
            if (fd < 0)
            {
                break;
            }

            co->Launch<ChatHandler>(fd, &broadcast, &registry);
            acceptCtx->Yield();
        }
    });

    // Broadcaster: reads from the broadcast channel and fans out to all connected clients
    //
    coop::RecvChannel<Message>* bcastRecv = &broadcast;
    co->Spawn([bcastRecv, &registry](coop::Context* bcastCtx)
    {
        bcastCtx->SetName("Broadcaster");
        Message msg;

        while (bcastRecv->Recv(bcastCtx, msg))
        {
            for (int32_t i = 0; i < MAX_CLIENTS; i++)
            {
                if (!registry.clients[i].active)
                {
                    continue;
                }

                // Don't echo back to the sender
                //
                if (registry.clients[i].fd == msg.senderFd)
                {
                    continue;
                }

                registry.clients[i].outbound->TrySend(bcastCtx, msg);
            }
        }
    });

    // Status: periodic stats, same pattern as echo_server
    //
    co->Spawn([](coop::Context* statusCtx)
    {
        statusCtx->SetName("Status");
        coop::time::Sleeper s(
            statusCtx,
            statusCtx->GetCooperator()->GetTicker(),
            std::chrono::seconds(10));

        while (s.Sleep())
        {
            spdlog::info("cooperator total={} yielded={} blocked={}",
                statusCtx->GetCooperator()->ContextsCount(),
                statusCtx->GetCooperator()->YieldedCount(),
                statusCtx->GetCooperator()->BlockedCount());
            statusCtx->GetCooperator()->PrintContextTree();
        }
    });

    // Yield until the shutdown handler shuts us down
    //
    while (!co->IsShuttingDown())
    {
        ctx->Yield(true);
    }
    spdlog::info("shutting down...");

    // Shut down the broadcast channel so the broadcaster exits cleanly
    //
    broadcast.Shutdown(ctx);
}

int main()
{
    coop::InstallShutdownHandler();
    coop::Cooperator cooperator;
    coop::Thread mt(&cooperator);

    cooperator.Submit(&SpawningTask);
    return 0;
}
