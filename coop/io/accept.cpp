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

bool Accept(Handle& handle, Descriptor& desc)
{
    auto* sqe = io_uring_get_sqe(&desc.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }

    // In this flavor, throwing these away
    //
    static struct ::sockaddr_in addrIn;
    static socklen_t addrLen;

    spdlog::trace("accept fd={}", desc.m_fd);
    io_uring_prep_accept(sqe, desc.m_fd, (struct sockaddr*)&addrIn, &addrLen, 0);
    handle.Submit(sqe);
    return true;
}

int Accept(Descriptor& desc)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Accept(handle, desc))
    {
        return -EAGAIN;
    }
    int fd = handle;
    spdlog::debug("accept fd={} accepted_fd={}", desc.m_fd, fd);
    return fd;
}

} // end namespace coop::io
} // end namespace coop
