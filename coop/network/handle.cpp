#include <cassert>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "handle.h"

#include "event_mask.h"
#include "router.h"

namespace coop
{

namespace network
{

Handle::Handle(int fd, EventMask mask, Coordinator* coordinator)
: m_fd(fd)
, m_mask(mask)
, m_coordinator(coordinator)
, m_router(nullptr)
{
}

bool Handle::SetNonBlocking()
{
    if (fcntl(GetFD(), F_SETFL, fcntl(GetFD(), F_GETFL) | O_NONBLOCK) == 0)
    {
        return true;
    }

    return false;
}

void Handle::Unregister()
{
    assert(Registered());
    m_router->Unregister(this);
    m_router = nullptr;
}

bool Handle::Registered() const
{
    return !!m_router;
}

EventMask Handle::GetMask() const
{
    return m_mask;
}

bool Handle::SetMask(EventMask mask)
{
    assert(Registered());

    if (!m_router->Update(this, mask))
    {
        return false;
    }

    m_mask = mask;
    return true;
}

bool Handle::operator+=(EventMask mask)
{
    return SetMask(m_mask | mask);
}

bool Handle::operator-=(EventMask mask)
{
    return SetMask(m_mask & ~mask);
}

int Handle::GetFD() const
{
    return m_fd;
}

void Handle::SetRouter(Router* router)
{
    assert(!Registered());
    m_router = router;
}

Coordinator* Handle::GetCoordinator()
{
    return m_coordinator;
}

} // end namespace coop::network

} // end namespace coop
