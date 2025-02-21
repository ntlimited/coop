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
                m_factory->Delete(handler);
                continue;
            }

            if (!ctx->GetCooperator()->Launch(*handler))
            {
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
