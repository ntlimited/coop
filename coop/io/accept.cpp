#include <sys/socket.h>
#include <netinet/in.h>

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

Handle Accept(Context* ctx, Coordinator* coord, Descriptor& desc)
{
    auto* sqe = io_uring_get_sqe(&desc.m_ring->m_ring);
    if (!sqe)
    {
        return Handle(0);
    }

    // In this flavor, throwing these away
    //
    static struct ::sockaddr_in addrIn;
    static socklen_t addrLen;

    io_uring_prep_accept(sqe, desc.m_fd, (struct sockaddr*)&addrIn, &addrLen, 0);
    return Handle(ctx, &desc, coord, sqe);
}

int Accept(Context* ctx, Descriptor& desc)
{
    Coordinator coord;
    return Accept(ctx, &coord, desc);
}

int Accept(Descriptor& desc)
{
    return Accept(Self(), desc);
}

} // end namespace coop::io
} // end namespace coop
