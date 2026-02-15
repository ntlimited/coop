#include "recv.h"

#include <cerrno>
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

bool Recv(Handle& handle, void* buf, size_t size, int flags /* = 0 */)
{
    auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }

    spdlog::trace("recv fd={} maxsize={}", handle.m_descriptor->m_fd, size);
    io_uring_prep_recv(sqe, handle.m_descriptor->m_fd, buf, size, flags);
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

    int result = handle;
    spdlog::trace("recv fd={} maxsize={} result={}", desc.m_fd, size, result);
    return result;
}

int Recv(Descriptor& desc, void* buf, size_t size, time::Interval timeout, int flags /* = 0 */)
{
    auto* sqe = io_uring_get_sqe(&desc.m_ring->m_ring);
    if (!sqe)
    {
        return -EAGAIN;
    }

    struct __kernel_timespec ts;
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    ts.tv_sec = secs.count();
    ts.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs).count();

    spdlog::trace("recv fd={} maxsize={} timeout={}ms", desc.m_fd, size, timeout.count());
    io_uring_prep_recv(sqe, desc.m_fd, buf, size, flags);

    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    handle.SubmitLinked(sqe, &ts);

    int result = handle;
    if (handle.TimedOut())
    {
        return -ETIMEDOUT;
    }
    return result;
}

} // end namespace coop::io
} // end namespace coop
