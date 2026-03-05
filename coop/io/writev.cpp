#include "writev.h"

#include <cerrno>

#include "coop/coordinator.h"
#include "coop/self.h"

#include "descriptor.h"
#include "handle.h"
#include "detail/handle_extension.h"

namespace coop
{

namespace io
{

bool Writev(Handle& handle, const struct iovec* iov, int iovcnt)
{
    auto* sqe = detail::HandleExtension::GetSqe(handle);
    if (!sqe)
    {
        return false;
    }
    io_uring_prep_writev(sqe, detail::HandleExtension::Fd(handle), iov, iovcnt, 0);
    handle.Submit(sqe);
    return true;
}

int Writev(Descriptor& desc, const struct iovec* iov, int iovcnt)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Writev(handle, iov, iovcnt))
    {
        return -EAGAIN;
    }
    return handle.Wait();
}

int WritevAll(Descriptor& desc, struct iovec* iov, int iovcnt)
{
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++)
    {
        total += iov[i].iov_len;
    }

    size_t sent = 0;
    while (iovcnt > 0)
    {
        int n = Writev(desc, iov, iovcnt);
        if (n <= 0)
        {
            return n;
        }
        sent += n;

        // Advance past fully-sent iovecs
        //
        size_t remaining = n;
        while (iovcnt > 0 && remaining >= iov->iov_len)
        {
            remaining -= iov->iov_len;
            iov++;
            iovcnt--;
        }

        // Adjust partially-sent iovec
        //
        if (remaining > 0 && iovcnt > 0)
        {
            iov->iov_base = static_cast<char*>(iov->iov_base) + remaining;
            iov->iov_len -= remaining;
        }
    }

    return static_cast<int>(sent);
}

} // end namespace coop::io
} // end namespace coop
