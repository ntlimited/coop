#include <cerrno>
#include <cstdio>
#include <cassert>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

#include "tcp_server.h"

#include "coop/context.h"
#include "coop/cooperator.h"

namespace coop
{

namespace network
{

TCPHandler::TCPHandler(int fd)
: m_handle(fd, IN | OUT | HUP | ERR, &m_coordinator)
{
}

TCPHandler::~TCPHandler()
{
    close(m_handle.GetFD());
    if (m_handle.Registered())
    {
        m_handle.Unregister();
    }
}

bool TCPHandler::Register(Router* r)
{
    if (!m_handle.SetNonBlocking())
    {
        return false;
    }

    if (!r->Register(&m_handle))
    {
        return false;
    }

    return true;
}

void TCPHandler::Launch(Context* ctx)
{
    char buffer[4096];
        
    printf("Launching tcp handler\n");

    while (1)
    {
        int ret = recv(m_fd, &buffer[0], 4096, 0);
        printf("Recv'd %d bytes\n", ret);
        if (ret <= 0)
        {
            if (ret & (EWOULDBLOCK | EAGAIN))
            {
                // Wait for another signal
                //
                m_coordinator.Acquire(ctx);
                continue;
            }
            break;
        }

        if (!Recv(ctx, &buffer[0], ret))
        {
            break;
        }
    }
  exit:
    printf("Exiting tcp handler\n");
    delete this;
}

bool TCPHandler::Send(Context* ctx, void* buffer, size_t bytes)
{
    printf("Sending %lu bytes of data\n", bytes);
    int remaining = bytes;
    auto* sending = reinterpret_cast<char*>(buffer);

    // For non-edge triggered mechanisms, it is untenable to listen for write events until actually
    // writing data. For that reason, we check if we're not already listening for them and them
    // switch back by the time we finish.
    //
    bool alteredMask = false;
    if (!(m_handle.GetMask() & OUT))
    {
        if (!(m_handle += OUT))
        {
            return false;
        }
        alteredMask = true;
    }

    while (remaining)
    {
        int sent = send(m_fd, sending, remaining, 0);
        if (sent < 0)
        {
            if (sent & (EAGAIN | EWOULDBLOCK))
            {
                // Wait on the coordinator again
                //
                m_coordinator.Acquire(ctx);
                continue;
            }

            if (alteredMask)
            {
                m_handle -= OUT;
            }
            return false;
        }
        remaining -= sent;
        sending += sent;
    }
                
    if (alteredMask)
    {
        m_handle -= OUT;
    }
    return true;
}

TCPServer::TCPServer(int fd, TCPHandlerFactory* factory)
: m_handle(fd, IN | HUP | ERR, &m_coordinator)
, m_factory(factory)
, m_router(nullptr)
{
}

TCPServer::~TCPServer()
{
    m_handle.Unregister();
    close(m_handle.GetFD());
}

bool TCPServer::Register(Router* r)
{
    if (!m_handle.SetNonBlocking())
    {
        return false;
    }

    if (!r->Register(&m_handle))
    {
        return false;
    }
    
    m_router = r;
    return true;
}

void TCPServer::Launch(Context* ctx)
{
    while (!ctx->IsKilled())
    {
        int fd = accept(m_handle.GetFD(), nullptr, nullptr);
        if (fd >= 0)
        {
            auto* handler = m_factory->Handler(fd);
            if (!handler)
            {
                close(fd);
                continue;
            }
            if (!handler->Register(m_router))
            {
                printf("Handler failed to register: %d (%s)\n", errno, strerror(errno));
                m_factory->Delete(handler);
                continue;
            }

            printf("Handler succeeded in registering\n");

            if (!ctx->GetCooperator()->Launch(*handler))
            {
                printf("Failed to launch handler\n");
                m_factory->Delete(handler);
            }

            continue;
        }

        if (errno & (EAGAIN | EWOULDBLOCK))
        {
            m_coordinator.Acquire(ctx);
            continue;
        }

        break;
    }
}

} // end namespace coop::network

} // end namespace coop
