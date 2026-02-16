#pragma once

#include <liburing.h>

#include "coop/io/handle.h"
#include "coop/io/descriptor.h"
#include "coop/io/uring.h"

namespace coop
{

namespace io
{

namespace detail
{

// HandleExtension launders access to Handle's private fields for use by IO operation macros
// and implementation files. Follows the CoordinatorExtension pattern.
//
struct HandleExtension
{
    static struct io_uring_sqe* GetSqe(Handle& h)
    {
        return io_uring_get_sqe(&h.m_ring->m_ring);
    }

    static int Fd(Handle& h)
    {
        return h.m_descriptor->m_fd;
    }

    static struct __kernel_timespec* Timeout(Handle& h)
    {
        return &h.m_timeout;
    }
};

} // end namespace detail
} // end namespace coop::io
} // end namespace coop
