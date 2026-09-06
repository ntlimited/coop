#include <cassert>
#include <cerrno>
#include <cstdint>
#include <liburing.h>

#include "armed_handle.h"
#include "armed_accept.h"
#include "detail/armed_handle_impl.hpp"

#include "buffer_ring.h"
#include "descriptor.h"
#include "uring.h"

namespace coop
{

namespace io
{

namespace detail
{

void ArmedDispatch(struct io_uring_cqe* cqe, uintptr_t data)
{
    if (data & kSpeciesTag)
    {
        ArmedAccept::Dispatch(cqe, data);
    }
    else
    {
        ArmedHandle::Dispatch(cqe, data);
    }
}

} // end namespace coop::io::detail

template struct ArmedHandleImpl<ArmedHandle, ArmedRecvEntry>;

ArmedHandle::ArmedHandle(
    Context* context,
    Descriptor& descriptor,
    BufferRing* bufferRing,
    Coordinator* coordinator)
: ArmedHandleImpl<ArmedHandle, ArmedRecvEntry>(context, descriptor, coordinator)
, m_bufferRing(bufferRing)
{
}

ArmedHandle::~ArmedHandle()
{
    TeardownDrain();

    bool returnedBuffer = false;
    if (m_returnBid >= 0)
    {
        m_bufferRing->Return(uint32_t(m_returnBid));
        m_returnBid = -1;
        returnedBuffer = true;
    }

    // Unread chunks still own pool buffers. The pool can serve other connections after
    // this handle is gone, so return queued buffers as well as the last consumed chunk.
    // TeardownDrain has stopped callbacks before we walk the queue.
    //
    while (m_qCount > 0)
    {
        Slot s = DequeueSlot();
        if (s.entry.bid >= 0)
        {
            m_bufferRing->Return(uint32_t(s.entry.bid));
            returnedBuffer = true;
        }
    }
    if (returnedBuffer)
    {
        m_bufferRing->Publish();
    }
}

void ArmedHandle::PrepSqe(struct io_uring_sqe* sqe)
{
    // A multishot recv that names only the buffer group: the kernel selects a pool
    // buffer per delivery and reports its id in cqe->flags. No userspace recv buffer is
    // pinned.
    //
    io_uring_prep_recv_multishot(sqe, m_descriptor->m_fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = m_bufferRing->Group();
}

uint32_t ArmedHandle::QueueBound() const
{
    return m_bufferRing->Entries();
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
        self->OnCqe(cqe);
    }
}

void ArmedHandle::OnCqe(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    uint32_t bid = 0;
    bool hasBuf = BufferRing::SelectedBuffer(cqe, &bid);

    // The CQ head is not advanced here. Uring::Poll reaps the whole ready batch and
    // issues a single io_uring_cq_advance after the last callback returns; a per-CQE
    // io_uring_cqe_seen would double-advance the kernel head. cqe stays valid for the
    // duration of this callback, which is all the buffer-id and result reads require.
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
        // Draining toward destruction: surface nothing, hand any buffer straight back,
        // and wake the destructor's Flash once everything outstanding has drained.
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
        // The pool drained and the kernel disarmed the multishot. Surface a terminal so
        // the caller recycles consumed buffers and calls Arm() to resume.
        //
        m_enobufs++;
        m_finalResult = res;
        EnqueueSlot(Entry{nullptr, -1}, res);
        WakeConsumer();
        return;
    }

    if (res < 0)
    {
        m_finalResult = res;
        EnqueueSlot(Entry{nullptr, -1}, res);
        WakeConsumer();
        return;
    }

    // res >= 0: a data chunk (res > 0) or EOF (res == 0).
    //
    char* data = hasBuf ? m_bufferRing->Buffer(bid) : nullptr;
    EnqueueSlot(Entry{data, hasBuf ? int32_t(bid) : -1}, res);

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
            // Benign multishot termination with data still flowing — re-arm
            // transparently so the connection keeps receiving.
            //
            Arm();
        }
    }

    WakeConsumer();
}

void ArmedHandle::OnCancelAck(struct io_uring_cqe* cqe)
{
    // CQ-head advance is deferred to Uring::Poll's batch io_uring_cq_advance (see
    // OnCqe); this callback only acknowledges the cancel and drains toward teardown.
    //
    (void)cqe;
    m_cancelPending = false;
    m_ring->m_pendingOps--;
    MaybeReleaseForTeardown();
}

int ArmedHandle::Next(Chunk* out)
{
    // Recycle the buffer handed out by the previous Next() now that the caller is done
    // with it.
    //
    if (m_returnBid >= 0)
    {
        m_bufferRing->ReturnAndPublish(uint32_t(m_returnBid));
        m_returnBid = -1;
    }

    Entry entry;
    int res = NextSlot(&entry);

    if (entry.bid >= 0)
    {
        m_returnBid = entry.bid;
    }
    *out = Chunk{entry.data, res, entry.bid};
    return res;
}

} // end namespace io
} // end namespace coop
