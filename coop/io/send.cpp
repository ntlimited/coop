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

bool Send(Handle& handle, void* buf, size_t size, int flags /* = 0 */)
{
    auto* sqe = io_uring_get_sqe(&handle.m_descriptor.m_ring->m_ring);
    if (!sqe)
    {
        // Try again?
        //
        return false;
    }

    io_uring_prep_send(sqe, handle.m_descriptor.m_fd, buf, size, flags);
    handle.Submit(sqe);
    return true;
}

int Send(Descriptor& desc, void* buf, size_t size, int flags /* = 0 */)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Send(handle, buf, size, flags))
    {
        return -EAGAIN;
    }

    return handle;
}

} // end namespace coop::io
} // end namespace coop
