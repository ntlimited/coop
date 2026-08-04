#include "send_zc.h"

#include <cerrno>
#include <liburing.h>

#include "coop/coordinator.h"
#include "coop/self.h"

#include "descriptor.h"
#include "detail/handle_extension.h"
#include "handle.h"
#include "uring.h"

namespace coop
{

namespace io
{

static int SendZCImpl(Descriptor& desc, const void* buf, size_t size, int flags,
                      const FixedBuffer* fixed, bool killAware)
{
    Coordinator coord;
    Handle handle(Self(), desc, &coord);

    auto* sqe = detail::HandleExtension::GetSqe(handle);
    if (!sqe)
    {
        return -EAGAIN;
    }

    if (fixed)
    {
        io_uring_prep_send_zc_fixed(sqe, detail::HandleExtension::Fd(handle),
                                    fixed->data, size, flags, 0, fixed->index);
    }
    else
    {
        io_uring_prep_send_zc(sqe, detail::HandleExtension::Fd(handle),
                              buf, size, flags, 0);
    }

    handle.Submit(sqe, 2);
    return killAware ? handle.WaitKill() : handle.Wait();
}

int SendZC(Descriptor& desc, const void* buf, size_t size, int flags /* = 0 */)
{
    return SendZCImpl(desc, buf, size, flags, nullptr, false);
}

int SendZCKill(Descriptor& desc, const void* buf, size_t size, int flags /* = 0 */)
{
    return SendZCImpl(desc, buf, size, flags, nullptr, true);
}

int SendZC(Descriptor& desc, FixedBuffer buf, size_t size, int flags /* = 0 */)
{
    return SendZCImpl(desc, nullptr, size, flags, &buf, false);
}

int SendZCKill(Descriptor& desc, FixedBuffer buf, size_t size, int flags /* = 0 */)
{
    return SendZCImpl(desc, nullptr, size, flags, &buf, true);
}

} // end namespace coop::io
} // end namespace coop
