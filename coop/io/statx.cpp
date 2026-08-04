#define COOP_IO_KEEP_ARGS
#include "statx.h"

#include <cerrno>
#include <fcntl.h>
#include <linux/stat.h>

#include "coop/coordinator.h"
#include "coop/self.h"

#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

COOP_IO_URING_IMPLEMENTATIONS(Statx, io_uring_prep_statx, STATX_ARGS)

// The fd form points statx at the file descriptor itself via AT_EMPTY_PATH — the macro
// families assume either a Descriptor or AT_FDCWD, so these are written out by hand in
// the same shape as the uring-level blocking wrappers.
//
static bool StatxFdAsync(Handle& handle, int fd, unsigned mask, struct statx* buf)
{
    auto* sqe = detail::HandleExtension::GetSqe(handle);
    if (!sqe)
    {
        return false;
    }
    io_uring_prep_statx(sqe, fd, "", AT_EMPTY_PATH, mask, buf);
    handle.Submit(sqe);
    return true;
}

int StatxFd(int fd, unsigned mask, struct statx* buf)
{
    auto* ring = GetUring();
    Coordinator coord;
    Handle handle(Self(), ring, &coord);
    if (!StatxFdAsync(handle, fd, mask, buf))
    {
        return -EAGAIN;
    }
    return handle.Wait();
}

int StatxFdKill(int fd, unsigned mask, struct statx* buf)
{
    auto* ring = GetUring();
    Coordinator coord;
    Handle handle(Self(), ring, &coord);
    if (!StatxFdAsync(handle, fd, mask, buf))
    {
        return -EAGAIN;
    }
    return handle.WaitKill();
}

} // end namespace coop::io
} // end namespace coop
