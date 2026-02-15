#include "timeout.h"

#include <liburing.h>

#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

bool Timeout(Handle& handle, time::Interval interval)
{
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(interval);
    handle.m_timeout.tv_sec = secs.count();
    handle.m_timeout.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(
        interval - secs).count();

    auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }
    io_uring_prep_timeout(sqe, &handle.m_timeout, 0, 0);
    handle.Submit(sqe);
    return true;
}

} // end namespace coop::io
} // end namespace coop
