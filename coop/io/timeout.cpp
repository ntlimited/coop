#include "timeout.h"

#include <liburing.h>

#include "handle.h"
#include "detail/handle_extension.h"

namespace coop
{

namespace io
{

bool Timeout(Handle& handle, time::Interval interval)
{
    auto* ts = detail::HandleExtension::Timeout(handle);
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(interval);
    ts->tv_sec = secs.count();
    ts->tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(
        interval - secs).count();

    auto* sqe = detail::HandleExtension::GetSqe(handle);
    if (!sqe)
    {
        return false;
    }
    io_uring_prep_timeout(sqe, ts, 0, 0);
    handle.Submit(sqe);
    return true;
}

} // end namespace coop::io
} // end namespace coop
