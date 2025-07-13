#include <cerrno>
#include <sys/socket.h>
#include <string.h>

#include "socket.h"

#include "router.h"

#include "coop/multi_coordinator.h"

namespace coop
{

namespace network
{

namespace
{

bool BlockAndRetry()
{
    return errno & (EAGAIN | EWOULDBLOCK | EINPROGRESS);
}

} // end anonymous namespace

Socket::Socket(int fd)
: m_handle(fd, IN | OUT | HUP | ERR, &m_coord)
{
    m_handle.SetNonBlocking();
}

// TODO decide this contract more fully
//
Socket::~Socket()
{
    if (m_handle.Registered())
    {
        m_handle.Unregister();
    }
}

bool Socket::Register(Router* router)
{
    return router->Register(&m_handle);
}

int Socket::Recv(Context* context, void* buffer, size_t len, int flags /* = MSG_DONTWAIT */)
{
    while (true)
    {
        int ret = recv(m_handle.GetFD(), buffer, len, flags);
        if (ret > 0)
        {
            return ret;
        }
        if (ret < 0)
        {
            printf("Recv (%d): %d (%s)\n", m_handle.GetFD(), errno, strerror(errno));
            if (!BlockAndRetry())
            {
                return ret;
            }
            return 0;
        }
        if (flags & MSG_DONTWAIT)
        {
            printf("Recv (%d): returning immediately from short read (DONTWAIT)\n", m_handle.GetFD());
            return 0;
        }

        printf("Blocking during Recv on %d\n", m_handle.GetFD());
        if (&m_coord != CoordinateWithKill(context, &m_coord))
        {
            return -1;
        }
        printf("Finished blocking\n");
    }
    assert(false);
    return -1;
}

int Socket::Send(Context* context, const void* buffer, size_t len, int flags /* = 0 */)
{
    while (true)
    {
        int ret = send(m_handle.GetFD(), buffer, len, flags);
        if (ret > 0)
        {
            return ret;
        }
        if (ret < 0)
        {
            printf("Send: %d (%s)\n", errno, strerror(errno));
            if (!BlockAndRetry())
            {
                return ret;
            }
        }

        printf("Blocking during Send\n");
        if (&m_coord != CoordinateWithKill(context, &m_coord))
        {
            return -1;
        }
        printf("Done blocking\n");
    }
    assert(false);
    return -1;
}

int Socket::SendAll(Context* context, const void* buffer, size_t len, int flags /* = 0 */)
{
    while (len)
    {
        int ret = Send(context, buffer, len, flags);
        if (ret < 0)
        {
            return ret;
        }

        buffer = ((const uint8_t*)buffer) + ret;
        len -= ret;

        if (&m_coord != CoordinateWithKill(context, &m_coord))
        {
            return -1;
        }
    }
    return 0;
}

Coordinator* Socket::GetCoordinator()
{
    return &m_coord;
}

} // end namespace coop::network
} // end namespace coop
