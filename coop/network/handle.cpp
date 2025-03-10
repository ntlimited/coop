#include <cassert>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "handle.h"

#include "event_mask.h"
#include "router.h"

#include "coop/cooperator.h"
#include "coop/context.h"
#include "coop/self.h"
#include "coop/coordinator_extension.h"
#include "coop/multi_coordinator.h"
#include "coop/time/sleep.h"

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

// Similar to other logic, better to make sure you're cleaning up, add an auto class if you want
// hand holding
Handle::~Handle()
{
    assert(!Registered());
    assert(Disconnected());
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
    m_router->m_list.Remove(this);
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

bool Handle::Wait(Context* ctx)
{
    if (CoordinateWithKill(m_coordinator) == m_coordinator)
    {
        return true;
    }

    // We were killed
    //
    return false;
}

bool Handle::Wait(Context* ctx, time::Interval i)
{
    auto* t = ctx->GetCooperator()->GetTicker();
    if (!t)
    {
        return false;
    }

    time::Sleeper s(ctx, t, i);
    s.Submit();
    Coordinator* acq = CoordinateWithKill(ctx, m_coordinator, s.GetCoordinator());
    return acq == m_coordinator;
}

int Handle::GetFD() const
{
    return m_fd;
}

void Handle::SetRouter(Router* router)
{
    assert(!Registered());
    m_router = router;
    m_router->m_list.Push(this);
}

Coordinator* Handle::GetCoordinator()
{
    return m_coordinator;
}

Handle::Data& Handle::GetData()
{
    return m_data;
}

} // end namespace coop::network

} // end namespace coop
