#include "read.h"

#include <spdlog/spdlog.h>

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

bool Read(Handle& handle, void* buf, size_t size, uint64_t offset /* = 0 */)
{
    auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }

    spdlog::trace("read fd={} maxsize={} offset={}", handle.m_descriptor->m_fd, size, offset);
    io_uring_prep_read(sqe, handle.m_descriptor->m_fd, buf, size, offset);
    handle.Submit(sqe);
    return true;
}

int Read(Descriptor& desc, void* buf, size_t size, uint64_t offset /* = 0 */)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Read(handle, buf, size, offset))
    {
        return -EAGAIN;
    }

    int result = handle;
    spdlog::trace("read fd={} maxsize={} offset={} result={}", desc.m_fd, size, offset, result);
    return result;
}

} // end namespace coop::io
} // end namespace coop
