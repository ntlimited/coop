#pragma once

#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/io/sendfile.h"
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

    // Keep-alive HTTP usually has the next pipelined request already buffered and the response
    // socket writable, so this transport opts into the recv/send fastpaths explicitly -- the case
    // the speculative nonblocking syscall is built for.
    //
    int Recv(void* buf, size_t size, int flags, time::Interval timeout)
    {
        if (timeout.count() > 0)
        {
            return io::RecvFastpath(m_desc, buf, size, flags, timeout);
        }
        return io::RecvFastpath(m_desc, buf, size, flags);
    }

    int SendAll(const void* buf, size_t size)
    {
        return io::SendAllFastpath(m_desc, buf, size);
    }

    int SendfileAll(int in_fd, off_t offset, size_t count)
    {
        return io::SendfileAll(m_desc, in_fd, offset, count);
    }

    io::Descriptor& m_desc;
};

} // end namespace coop::http
} // end namespace coop
