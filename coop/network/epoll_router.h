#pragma once

#include <sys/epoll.h>
#include <string.h>
#include <vector>

#include "router.h"

#include "coop/launchable.h"

namespace coop
{

struct Context;
struct Coordinator;

namespace network
{

// The EpollRouter uses the execution context framework to signal tasks when they have
// events on the submitted file descriptors
//
struct EpollRouter : Router
{
    using MaskConverter = InOutHupErrPri<uint32_t,EPOLLIN,EPOLLOUT,EPOLLHUP,EPOLLPRI,EPOLLERR>;
    static constexpr size_t MAX_EVENTS = 512;

    // The EpollRouter is given an already configured (`epoll_create`'d) fd to operate on.
    //
    EpollRouter(int epollFd);

    // The router must be launched before it is used
    //
    void Launch(Context* ctx) final;

    bool Register(Handle*) final;

  private:
    bool Unregister(Handle*) final;
    bool Update(Handle*, const EventMask) final;

  private:
    Context*            m_ctx;
    int                 m_epollFd;
    struct epoll_event  m_events[MAX_EVENTS];
};

} // end namespace coop::network

} // end namespace coop
