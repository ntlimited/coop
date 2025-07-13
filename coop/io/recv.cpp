#include "recv.h"

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

bool Recv(Handle& handle, void* buf, size_t size, int flags /* = 0 */)
{
    auto* sqe = io_uring_get_sqe(&handle.m_descriptor.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }

    io_uring_prep_recv(sqe, handle.m_descriptor.m_fd, buf, size, flags);
    handle.Submit(sqe);
    return true;
}

int Recv(Descriptor& desc, void* buf, size_t size, int flags /* = 0 */)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Recv(handle, buf, size, flags))
    {
        return -EAGAIN;
    }

    return handle;
}

} // end namespace coop::io
} // end namespace coop
