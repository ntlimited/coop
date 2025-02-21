#pragma once

#include <cstdint>
#include <sys/epoll.h>

namespace coop
{

namespace network
{

struct EventMask
{
    constexpr EventMask operator|(const EventMask other) const
    {
        return EventMask{m_mask | other.m_mask};
    }

    constexpr EventMask operator&(const EventMask other) const
    {
        return EventMask{m_mask & other.m_mask};
    }

    constexpr EventMask operator~() const
    {
        return EventMask{~m_mask};
    }

    operator bool() const
    {
        return !!m_mask;
    }

    uint64_t m_mask;
};
    
// The 'ready to recv' event for the fd
//
static constexpr EventMask IN = EventMask{EPOLLIN};

// The 'ready to send' event for the fd
//
static constexpr EventMask OUT = EventMask{EPOLLOUT};

// The 'the socket hung up' event for the fd
//
static constexpr EventMask HUP = EventMask{EPOLLHUP};

// The 'there was an error' event for the fd
//
static constexpr EventMask ERR = EventMask{EPOLLERR};

// The 'exceptional condition' event for the fd
//
static constexpr EventMask PRI = EventMask{EPOLLPRI};

// InOutHupErrPri allows mapping the "generic" event mask from coop::network into whatever flavor
// is needed by the router implementation (EPOLLOUT, POLLOUT, etc).
//
template<typename T, T IN, T OUT, T HUP, T ERR, T PRI>
struct InOutHupErrPri
{
    constexpr T Convert(const EventMask mask) const
    {
        T res = 0;
        if (mask & ::coop::network::IN)     res |= IN;
        if (mask & ::coop::network::OUT)    res |= OUT;
        if (mask & ::coop::network::HUP)    res |= HUP;
        if (mask & ::coop::network::ERR)    res |= ERR;
        if (mask & ::coop::network::PRI)    res |= PRI;
        return res;
    }

    constexpr T operator()(const EventMask mask) const
    {
        return Convert(mask);
    }
};

} // end namespace coop::network

} // end namespace coop
