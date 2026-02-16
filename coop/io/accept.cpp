#include <sys/socket.h>
#include <netinet/in.h>

#include <spdlog/spdlog.h>

#include "accept.h"

#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/self.h"

#include "descriptor.h"
#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

bool Accept(Handle& handle, struct sockaddr* addr, socklen_t* addrLen)
{
    auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }

    spdlog::trace("accept fd={}", handle.m_descriptor->m_fd);
    io_uring_prep_accept(sqe, handle.m_descriptor->m_fd, addr, addrLen, 0);
    handle.Submit(sqe);
    return true;
}

int Accept(Descriptor& desc, struct sockaddr* addr, socklen_t* addrLen)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Accept(handle, addr, addrLen))
    {
        return -EAGAIN;
    }
    int fd = handle;
    spdlog::debug("accept fd={} accepted_fd={}", desc.m_fd, fd);
    return fd;
}

} // end namespace coop::io
} // end namespace coop
