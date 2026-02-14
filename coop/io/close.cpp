#include <spdlog/spdlog.h>

#include "close.h"

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

bool Close(Handle& handle)
{
    auto* sqe = io_uring_get_sqe(&handle.m_descriptor.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }

    spdlog::trace("close fd={}", handle.m_descriptor.m_fd);
    io_uring_prep_close(sqe, handle.m_descriptor.m_fd);
    handle.Submit(sqe);
    return true;
}

int Close(Descriptor& desc)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Close(handle))
    {
        return -EAGAIN;
    }

    int result = handle;
    spdlog::debug("close fd={} result={}", desc.m_fd, result);
    return result;
}

} // end namespace coop::io
} // end namespace coop
