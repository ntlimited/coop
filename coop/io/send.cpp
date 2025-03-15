#include "send.h"

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

Handle Send(Context* ctx, Coordinator* coord, Descriptor& desc, void* buf, size_t size, int flags /* = 0 */)
{
    auto* sqe = io_uring_get_sqe(&desc.m_ring->m_ring);
    if (!sqe)
    {
        // Try again?
        //
        return Handle(-EAGAIN);
    }

    io_uring_prep_send(sqe, desc.m_fd, buf, size, flags);
    return Handle(ctx, &desc, coord, sqe);
}

int Send(Context* ctx, Descriptor& desc, void* buf, size_t size, int flags /* = 0 */)
{
    Coordinator coord;
    return Send(ctx, &coord, desc, buf, size, flags);
}

int Send(Descriptor& desc, void* buf, size_t size, int flags /* = 0 */)
{
    return Send(Self(), desc, buf, size, flags);
}

} // end namespace coop::io
} // end namespace coop
