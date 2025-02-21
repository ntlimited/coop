#pragma once

#include <sys/epoll.h>
#include <string.h>
#include <vector>

#include "coordinator.h"
#include "launchable.h"

// The EpollController uses the execution context framework to signal tasks when they have
// events on the submitted file descriptors
//
struct EpollController : Launchable
{
    static constexpr size_t MAX_EVENTS = 512;

    // The EpollListener is given an already configured (`epoll_create`'d) fd to operate on.
    //
    EpollController(int epollFd)
    : m_epollFd(epollFd)
    {
    }

    // Launch is expected to be 
    void Launch(ExecutionContext* ctx) final
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

    // This registers epoll events 
    //
    bool Add(
        int fd,
        uint32_t events,
        Coordinator* coordinator)
    {
        struct epoll_event event;

        // We will only trigger (e.g. release the coordinator) once per event, even if not processed
        // immediately
        //
        event.events = events | EPOLLET;
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
        bool success = coordinator->TryAcquire(m_ctx);
        assert(success);
        std::ignore = success;
        return true;
    }

    bool Remove(int fd)
    {
        int res = epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);
        // TODO error handling
        //
        return res == 0;
    }

  private:
    ExecutionContext* m_ctx;
    int m_epollFd;
    struct epoll_event m_events[MAX_EVENTS];
};
