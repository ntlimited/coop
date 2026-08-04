#include <cassert>
#include <cerrno>
#include <cstdint>
#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>

#include "armed_accept.h"
#include "detail/armed_handle_impl.hpp"

#include "descriptor.h"
#include "uring.h"

namespace coop
{

namespace io
{

template struct ArmedHandleImpl<ArmedAccept, ArmedAcceptEntry>;

ArmedAccept::ArmedAccept(
    Context* context,
    Descriptor& listener,
    Coordinator* coordinator,
    uint32_t maxPending /* = 64 */)
: ArmedHandleImpl<ArmedAccept, ArmedAcceptEntry>(context, listener, coordinator)
, m_maxPending(maxPending)
{
    assert(maxPending > 0);
}

ArmedAccept::~ArmedAccept()
{
    TeardownDrain();

    // Surfaced-but-unconsumed connections are real kernel fds nobody will ever service.
    //
    while (m_qCount > 0)
    {
        Slot s = DequeueSlot();
        if (s.entry.fd >= 0)
        {
            ::close(s.entry.fd);
        }
    }
}

void ArmedAccept::PrepSqe(struct io_uring_sqe* sqe)
{
    io_uring_prep_multishot_accept(sqe, m_descriptor->m_fd, nullptr, nullptr,
                                   SOCK_NONBLOCK);
}

void ArmedAccept::Dispatch(struct io_uring_cqe* cqe, uintptr_t data)
{
    auto* self = reinterpret_cast<ArmedAccept*>(data & ~uintptr_t(0x7));
    if (data & kCancelTag)
    {
        self->OnCancelAck(cqe);
    }
    else
    {
        self->OnCqe(cqe);
    }
}

void ArmedAccept::OnCqe(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;

    if (!more)
    {
        m_armed = false;
        m_ring->m_pendingOps--;
    }

    if (m_tearingDown)
    {
        if (res >= 0)
        {
            ::close(res);
        }
        MaybeReleaseForTeardown();
        return;
    }

    if (res < 0)
    {
        // The terminal CQE of a backpressure Cancel is not an error — swallow it; the
        // drain side re-arms. A transient per-connection failure with the multishot
        // still live is likewise not terminal.
        //
        if (m_pausing && res == -ECANCELED)
        {
            TryResume();
            return;
        }
        if (more && (res == -ECONNABORTED || res == -EINTR))
        {
            return;
        }

        m_finalResult = res;
        EnqueueSlot(Entry{}, res);
        WakeConsumer();
        return;
    }

    // Past the burst-absorption capacity, shed: the cancel is in flight (or about to
    // be) but this Poll batch already carried more accepts than the queue may hold, and
    // each entry is a real kernel fd. Closing here sends the peer a reset — overload
    // shedding at the edge, counted for observability.
    //
    if (m_qCount >= QueueBound())
    {
        m_sheds++;
        ::close(res);
        if (!m_pausing)
        {
            m_pausing = true;
            m_pauses++;
            Cancel();
        }
        return;
    }

    m_delivered++;
    EnqueueSlot(Entry{res}, res);

    // Backpressure: at the pending bound, stop the kernel accepting into our queue —
    // the listen backlog takes over. Cancel's terminal CQE is swallowed above and the
    // consumer's drain re-arms. Checked before the benign re-arm so a kernel disarm at
    // high water pauses instead of resuming.
    //
    if (m_qCount >= m_maxPending)
    {
        if (!m_pausing)
        {
            m_pausing = true;
            m_pauses++;
            Cancel();
        }
    }
    else if (!m_armed)
    {
        // Benign kernel disarm with the stream healthy — re-arm transparently.
        //
        Arm();
    }

    WakeConsumer();
}

void ArmedAccept::OnCancelAck(struct io_uring_cqe* cqe)
{
    (void)cqe;
    m_cancelPending = false;
    m_ring->m_pendingOps--;

    if (m_tearingDown)
    {
        MaybeReleaseForTeardown();
        return;
    }
    TryResume();
}

void ArmedAccept::TryResume()
{
    // Resume only once both halves of the pause completed (terminal CQE and cancel
    // ack), the stream is healthy, and the consumer has drained below half the bound.
    //
    if (m_pausing && !m_armed && !m_cancelPending && m_finalResult == 0 &&
        m_qCount <= m_maxPending / 2)
    {
        m_pausing = false;
        Arm();
    }
}

int ArmedAccept::Next()
{
    Entry entry;
    int res = NextSlot(&entry);

    // Consuming may have opened room below the resume threshold while paused.
    //
    if (!m_tearingDown)
    {
        TryResume();
    }
    return res >= 0 ? entry.fd : res;
}

} // end namespace io
} // end namespace coop
