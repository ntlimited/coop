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

// Per-connection socket-readiness policy — the three-state answer to "will data usually
// be there when we recv?":
//
//   Fastpath  — data usually ready (keep-alive server sockets with pipelined requests):
//               a speculative nonblocking syscall wins ~500ns when it hits, wastes an
//               EAGAIN syscall when it misses.
//   Plain     — no opinion: straight to the ring.
//   PollFirst — data known-absent (request/response turnaround — a client that just
//               sent a request): IORING_RECVSEND_POLL_FIRST arms the poll before the
//               kernel even attempts the receive, skipping the guaranteed-empty attempt.
//
enum class RecvPolicy : uint8_t
{
    Fastpath,
    Plain,
    PollFirst,
};

// PlaintextTransport dispatches HTTP I/O directly through io_uring. Zero overhead — each method
// is a thin inline wrapper around the corresponding io:: free function.
//
struct PlaintextTransport
{
    // A bare socket: body bytes can move by splice without visiting userspace.
    //
    static constexpr bool kSpliceable = true;

    // Default is Fastpath — the keep-alive server shape this transport historically
    // opted into. Upstream client connections (send request, await response) should
    // pass RecvPolicy::PollFirst.
    //
    explicit PlaintextTransport(io::Descriptor& desc,
                                RecvPolicy policy = RecvPolicy::Fastpath)
    : m_desc(desc)
    , m_recvPolicy(policy)
    {}

    io::Descriptor& Descriptor() { return m_desc; }

    int Recv(void* buf, size_t size, int flags, time::Interval timeout)
    {
        switch (m_recvPolicy)
        {
            case RecvPolicy::Fastpath:
                if (timeout.count() > 0)
                {
                    return io::RecvFastpath(m_desc, buf, size, flags, timeout);
                }
                return io::RecvFastpath(m_desc, buf, size, flags);

            case RecvPolicy::PollFirst:
                if (timeout.count() > 0)
                {
                    return io::RecvPollFirst(m_desc, buf, size, flags, timeout);
                }
                return io::RecvPollFirst(m_desc, buf, size, flags);

            case RecvPolicy::Plain:
                break;
        }
        if (timeout.count() > 0)
        {
            return io::Recv(m_desc, buf, size, flags, timeout);
        }
        return io::Recv(m_desc, buf, size, flags);
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
    RecvPolicy      m_recvPolicy;
};

} // end namespace coop::http
} // end namespace coop
