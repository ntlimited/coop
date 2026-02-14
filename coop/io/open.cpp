#include "open.h"

#include <fcntl.h>

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

bool Open(Handle& handle, const char* path, int flags, mode_t mode /* = 0 */)
{
    auto* sqe = io_uring_get_sqe(&handle.m_ring->m_ring);
    if (!sqe)
    {
        return false;
    }

    spdlog::trace("open path={} flags={}", path, flags);
    io_uring_prep_openat(sqe, AT_FDCWD, path, flags, mode);
    handle.Submit(sqe);
    return true;
}

int Open(const char* path, int flags, mode_t mode /* = 0 */)
{
    auto* ring = GetUring();

    Coordinator coord;
    Handle handle(Self(), ring, &coord);
    if (!Open(handle, path, flags, mode))
    {
        return -EAGAIN;
    }

    int fd = handle;
    spdlog::debug("open path={} result={}", path, fd);
    return fd;
}

} // end namespace coop::io
} // end namespace coop
