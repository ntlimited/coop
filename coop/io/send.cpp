#include "send.h"

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

bool Send(Handle& handle, const void* buf, size_t size, int flags /* = 0 */)
{
    auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }

    spdlog::trace("send fd={} size={}", handle.m_descriptor->m_fd, size);
    io_uring_prep_send(sqe, handle.m_descriptor->m_fd, buf, size, flags);
    handle.Submit(sqe);
    return true;
}

int Send(Descriptor& desc, const void* buf, size_t size, int flags /* = 0 */)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Send(handle, buf, size, flags))
    {
        return -EAGAIN;
    }

    int result = handle;
    spdlog::trace("send fd={} size={} result={}", desc.m_fd, size, result);
    return result;
}

int SendAll(Descriptor& desc, const void* buf, size_t size, int flags /* = 0 */)
{
    size_t offset = 0;
    while (offset < size)
    {
        int sent = Send(desc, (const char*)buf + offset, size - offset, flags);
        if (sent <= 0)
        {
            return sent;
        }
        offset += sent;
    }
    return (int)size;
}

} // end namespace coop::io
} // end namespace coop
