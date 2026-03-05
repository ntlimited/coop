#pragma once

#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/io/sendfile.h"
#include "coop/io/writev.h"
#include "coop/time/interval.h"

namespace coop
{
namespace http
{

// PlaintextTransport dispatches HTTP I/O directly through io_uring. Zero overhead — each method
// is a thin inline wrapper around the corresponding io:: free function.
//
struct PlaintextTransport
{
    explicit PlaintextTransport(io::Descriptor& desc) : m_desc(desc) {}

    io::Descriptor& Descriptor() { return m_desc; }

    int Recv(void* buf, size_t size, int flags, time::Interval timeout)
    {
        if (timeout.count() > 0)
        {
            return io::Recv(m_desc, buf, size, flags, timeout);
        }
        return io::Recv(m_desc, buf, size, flags);
    }

    int SendAll(const void* buf, size_t size)
    {
        return io::SendAll(m_desc, buf, size);
    }

    int WritevAll(struct iovec* iov, int iovcnt)
    {
        return io::WritevAll(m_desc, iov, iovcnt);
    }

    int SendfileAll(int in_fd, off_t offset, size_t count)
    {
        return io::SendfileAll(m_desc, in_fd, offset, count);
    }

    io::Descriptor& m_desc;
};

} // end namespace coop::http
} // end namespace coop
