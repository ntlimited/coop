#include <cerrno>
#include <cstdio>
#include <cassert>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

#include "tcp_handler.h"
#include "tcp_server.h"

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/multi_coordinator.h"

namespace coop
{

namespace network
{

TCPHandler::TCPHandler(Context* ctx, int fd)
: Launchable(ctx)
, m_handle(fd, IN | HUP | ERR, &m_coordinator)
{
    ctx->SetName("TCPHandler");
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

void TCPHandler::Launch()
{
    char buffer[4096];
        
    while (!GetContext()->IsKilled())
    {
        int ret = recv(m_handle.GetFD(), &buffer[0], 4096, 0);
        if (ret <= 0)
        {
            printf("!! error in recv: %d (%s)\n", errno, strerror(errno));
            if (ret & (EWOULDBLOCK | EAGAIN))
            {
                // Wait for another signal
                //
                if (m_coordinator != CoordinateWithKill(GetContext(), &m_coordinator))
                {
                    // Our context was killed before our coordinator triggered
                    //
                    assert(GetContext()->IsKilled());
                    return;
                }
                continue;
            }
            break;
        }

        if (!Recv(GetContext(), &buffer[0], ret))
        {
            break;
        }
    }
}

bool TCPHandler::Send(Context* ctx, void* buffer, size_t bytes)
{
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
        // MSG_NOSIGNAL: ignore sigpipe. This came up when testing a delayed task that waits to echo
        // back data, and would still be alive after the TCPHandler itself was destructed. This could
        // probably be handled with more elegant lifetime management but it's not worth it for a toy
        // framework.
        //
        int sent = send(m_handle.GetFD(), sending, remaining, MSG_NOSIGNAL);
        if (sent < 0)
        {
            if (sent & (EAGAIN | EWOULDBLOCK))
            {
                // Wait on the coordinator again
                //
                if (&m_coordinator != CoordinateWithKill(ctx, &m_coordinator))
                {
                    break;
                }
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
    return remaining == 0;
}

} // end namespace network
} // end namespace coop
