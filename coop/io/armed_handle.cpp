#include <cassert>
#include <cerrno>
#include <cstdint>
#include <liburing.h>

#include "armed_handle.h"

#include "buffer_ring.h"
#include "descriptor.h"
#include "uring.h"

#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"

namespace coop
{

namespace io
{

// Tagged userdata for ArmedHandle CQEs. Bit 1 marks "armed" (routed to ArmedHandle by
// Handle::Callback); bit 0 then distinguishes the cancel acknowledgment from a recv CQE. The
// low 3 bits are free because the static_assert guarantees 8-byte alignment.
//
static constexpr uintptr_t kArmedTag  = 0x2;
static constexpr uintptr_t kCancelTag = 0x1;

ArmedHandle::ArmedHandle(
    Context* context,
    Descriptor& descriptor,
    BufferRing* bufferRing,
    Coordinator* coordinator)
: m_ring(descriptor.m_ring)
, m_descriptor(&descriptor)
, m_bufferRing(bufferRing)
, m_coord(coordinator)
, m_context(context)
{
    // The queue is allocated lazily on the first surfaced chunk -- see Enqueue. Sizing it here
    // would charge every armed connection O(pool) of resident memory whether or not any bytes
    // ever arrive, which is exactly the per-connection cost a buffer ring exists to avoid: an
    // idle keep-alive connection must hold essentially nothing.
    //
}

ArmedHandle::~ArmedHandle()
{
    m_tearingDown = true;

    if (m_armed)
    {
        // A multishot is still live in the kernel. Cancel it and cooperatively block until the
        // cancel acknowledgment and the terminal recv CQE drain; the drain path releases the
        // coordinator, which wakes this Flash. Mirrors io::Handle's destructor contract.
        //
        Cancel();
        m_coord->Flash(m_context);
    }
    else if (m_coord->IsHeld())
    {
        // The stream already ended (EOF / error) but the coordinator is still held from Arm().
        // Nothing is in flight, so release it directly.
        //
        m_coord->Release(m_context, false);
    }

    if (m_returnBid >= 0)
    {
        m_bufferRing->ReturnAndPublish(uint32_t(m_returnBid));
        m_returnBid = -1;
    }
}

void ArmedHandle::Arm()
{
    assert(!m_armed && "Arm() called while a multishot is already live");

    auto* sqe = m_ring->GetSqe();
    assert(sqe);

    // A multishot recv that names only the buffer group: the kernel selects a pool buffer per
    // delivery and reports its id in cqe->flags. No userspace recv buffer is pinned.
    //
    io_uring_prep_recv_multishot(sqe, m_descriptor->m_fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = m_bufferRing->Group();

    if (m_descriptor->m_registeredIndex >= 0)
    {
        sqe->fd = m_descriptor->m_registeredIndex;
        sqe->flags |= IOSQE_FIXED_FILE;
    }

    io_uring_sqe_set_data(sqe,
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) | kArmedTag));

    // No-op once the coordinator is already held (the steady state across re-arms).
    //
    m_coord->TryAcquire(m_context);
    m_ring->m_pendingOps++;
    m_armed = true;
}

void ArmedHandle::Cancel()
{
    if (!m_armed)
    {
        return;
    }

    auto* sqe = m_ring->GetSqe();
    assert(sqe);

    // Target the multishot SQE by its (armed-tagged) userdata. The cancel's own userdata carries
    // both the armed tag and the cancel tag so its completion routes to OnCancelAck.
    //
    io_uring_prep_cancel(sqe,
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) | kArmedTag), 0);
    io_uring_sqe_set_data(sqe,
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) | kArmedTag | kCancelTag));

    m_cancelPending = true;
    m_ring->m_pendingOps++;
}

void ArmedHandle::Dispatch(struct io_uring_cqe* cqe, uintptr_t data)
{
    auto* self = reinterpret_cast<ArmedHandle*>(data & ~uintptr_t(0x7));
    if (data & kCancelTag)
    {
        self->OnCancelAck(cqe);
    }
    else
    {
        self->OnRecv(cqe);
    }
}

void ArmedHandle::OnRecv(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    uint32_t bid = 0;
    bool hasBuf = BufferRing::SelectedBuffer(cqe, &bid);

    // The CQ head is not advanced here. Uring::Poll reaps the whole ready batch and issues a
    // single io_uring_cq_advance after the last callback returns; a per-CQE io_uring_cqe_seen
    // would double-advance the kernel head. cqe stays valid for the duration of this callback,
    // which is all the buffer-id and result reads above require.
    //

    if (!more)
    {
        // This multishot will produce no further CQEs.
        //
        m_armed = false;
        m_ring->m_pendingOps--;
    }

    if (m_tearingDown)
    {
        // Draining toward destruction: surface nothing, hand any buffer straight back, and wake
        // the destructor's Flash once everything outstanding has drained.
        //
        if (hasBuf)
        {
            m_bufferRing->ReturnAndPublish(bid);
        }
        MaybeReleaseForTeardown();
        return;
    }

    if (res == -ENOBUFS)
    {
        // The pool drained and the kernel disarmed the multishot. Surface a terminal so the
        // caller recycles consumed buffers and calls Arm() to resume. (A production path could
        // instead drop to a one-shot recv into a private buffer and re-arm transparently.)
        //
        m_enobufs++;
        m_finalResult = res;
        Enqueue(nullptr, res, -1);
        WakeConsumer();
        return;
    }

    if (res < 0)
    {
        m_finalResult = res;
        Enqueue(nullptr, res, -1);
        WakeConsumer();
        return;
    }

    // res >= 0: a data chunk (res > 0) or EOF (res == 0).
    //
    char* data = hasBuf ? m_bufferRing->Buffer(bid) : nullptr;
    Enqueue(data, res, hasBuf ? int32_t(bid) : -1);

    if (res == 0)
    {
        // Peer closed. Stay disarmed; the consumer sees a zero-length chunk.
        //
        m_finalResult = 0;
    }
    else
    {
        m_delivered++;
        if (!more)
        {
            // Benign multishot termination with data still flowing -- re-arm transparently so the
            // connection keeps receiving.
            //
            Arm();
        }
    }

    WakeConsumer();
}

void ArmedHandle::OnCancelAck(struct io_uring_cqe* cqe)
{
    // CQ-head advance is deferred to Uring::Poll's batch io_uring_cq_advance (see OnRecv); this
    // callback only acknowledges the cancel and drains toward teardown.
    //
    (void)cqe;
    m_cancelPending = false;
    m_ring->m_pendingOps--;
    MaybeReleaseForTeardown();
}

void ArmedHandle::WakeConsumer()
{
    if (m_consumerParked)
    {
        // Consume the park token before releasing so a burst of CQEs in one Poll wakes the
        // consumer exactly once and keeps the coordinator held across the hand-off.
        //
        m_consumerParked = false;
        m_coord->Release(m_context, false);
    }
}

void ArmedHandle::MaybeReleaseForTeardown()
{
    if (!m_armed && !m_cancelPending)
    {
        m_coord->Release(m_context, false);
    }
}

void ArmedHandle::Enqueue(char* data, int32_t len, int32_t bid)
{
    if (m_queue.empty())
    {
        // First chunk for this connection: size the queue to a hard bound it can never exceed.
        // At most pool-many buffers can be checked out at once (a checked-out buffer is one not
        // yet recycled), plus headroom for a terminal (EOF / error) marker.
        //
        m_queue.resize(m_bufferRing->Entries() + 4);
    }
    assert(m_qCount < m_queue.size());
    m_queue[m_qTail] = Chunk{data, len, bid};
    m_qTail = (m_qTail + 1) % m_queue.size();
    m_qCount++;
}

ArmedHandle::Chunk ArmedHandle::Dequeue()
{
    assert(m_qCount > 0);
    Chunk c = m_queue[m_qHead];
    m_qHead = (m_qHead + 1) % m_queue.size();
    m_qCount--;
    return c;
}

int ArmedHandle::Next(Chunk* out)
{
    // Recycle the buffer handed out by the previous Next() now that the caller is done with it.
    //
    if (m_returnBid >= 0)
    {
        m_bufferRing->ReturnAndPublish(uint32_t(m_returnBid));
        m_returnBid = -1;
    }

    while (m_qCount == 0)
    {
        if (!m_armed)
        {
            // Stream finished and nothing buffered: report the terminal result idempotently.
            //
            *out = Chunk{nullptr, 0, -1};
            return m_finalResult;
        }

        m_consumerParked = true;
        CoordinateWith(m_context, m_coord);
        m_consumerParked = false;
    }

    Chunk c = Dequeue();
    if (c.bid >= 0)
    {
        m_returnBid = c.bid;
    }
    *out = c;
    return c.len;
}

} // end namespace io
} // end namespace coop
