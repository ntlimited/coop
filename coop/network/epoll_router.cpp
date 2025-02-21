#include <cassert>
#include <cerrno>
#include <cstdio>

#include "epoll_router.h"
#include "handle.h"

#include "coop/context.h"
#include "coop/coordinator.h"

namespace coop
{

namespace network
{

EpollRouter::EpollRouter(int epollFd)
: m_epollFd(epollFd)
, m_ctx(nullptr)
{
}

void EpollRouter::Launch(Context* ctx)
{
    m_ctx = ctx;
    while (!ctx->IsKilled())
    {
        // For obvious reasons, we perform a non-blocking wait
        //
        int ret = epoll_wait(m_epollFd, &m_events[0], MAX_EVENTS, 0);

        // TODO do something about the error
        //
        if (!ret)
        {
            ctx->Yield();
            continue;
        }
        if (ret < 0)
        {
            printf("epoll_wait error: %d (%s)\n", errno, strerror(errno));
            ctx->Yield();
            continue;
        }

        int at = 0;
        while (at < ret)
        {
            auto& evt = m_events[at++];
            reinterpret_cast<Coordinator*>(evt.data.ptr)->Release(ctx);
        }

        ctx->Yield();
    }
}

// The epoll router stores its state internally and doesn't need to store anything in the handle
//
bool EpollRouter::Register(Handle* handle)
{
    int fd = handle->GetFD();
    EventMask mask = handle->GetMask();
    Coordinator* coordinator = GetCoordinator(handle);
    struct epoll_event event;

    // We will only trigger (e.g. release the coordinator) once per event, even if do not process it
    // immediately
    //
    event.events = MaskConverter()(mask) | EPOLLET;

    event.data.ptr = reinterpret_cast<void*>(coordinator);

    int res = epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &event);
        
    if (res < 0)
    {
        printf("epoll_ctl failed: %d (%s)\n", errno, strerror(errno));
        // TODO error handling
        //
        return false;
    }

    // We acquire the coordinator and will release it when we get events triggered off of epoll
    //
    coordinator->TryAcquire(m_ctx);

    // Handle will know to make future callbacks to this router
    //
    SetRouter(handle);
    return true;
}

bool EpollRouter::Unregister(Handle* handle)
{
    int res = epoll_ctl(m_epollFd, EPOLL_CTL_DEL, handle->GetFD(), nullptr);
    // TODO error handling
    //
    return res == 0;
}

bool EpollRouter::Update(Handle* handle, const EventMask mask)
{
    // Set up the new events, and the same data pointer as before
    //
    struct epoll_event event;
    event.data.ptr = reinterpret_cast<void*>(GetCoordinator(handle));
    event.events = MaskConverter()(mask) | EPOLLET;

    int res = epoll_ctl(m_epollFd, EPOLL_CTL_MOD, handle->GetFD(), &event);

    if (res != 0)
    {
        // TODO error handling
        //
        return false;
    }

    // Lock the coordinator before we hand back control so that we don't try to alert it
    //
    GetCoordinator(handle)->TryAcquire(m_ctx);
    return true;
}

} // end namespace coop::network

} // end namespace coop
