#include <cassert>
#include <cerrno>
#include <cstdint>
#include <liburing.h>
#include <spdlog/spdlog.h>

#include "handle.h"

#include "descriptor.h"
#include "uring.h"

#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"

namespace coop
{

namespace io
{

Handle::Handle(
    Context* context,
    Descriptor& descriptor,
    Coordinator* coordinator)
: m_ring(descriptor.m_ring)
, m_descriptor(&descriptor)
, m_coord(coordinator)
, m_context(context)
, m_result(0)
, m_pendingCqes(0)
, m_timedOut(false)
{
}

Handle::Handle(
    Context* context,
    Uring* ring,
    Coordinator* coordinator)
: m_ring(ring)
, m_descriptor(nullptr)
, m_coord(coordinator)
, m_context(context)
, m_result(0)
, m_pendingCqes(0)
, m_timedOut(false)
{
}

Handle::~Handle()
{
    if (m_pendingCqes > 0)
    {
        Cancel();
        m_coord->Flash(m_context);
    }
    assert(Disconnected());
}

void Handle::Submit(struct io_uring_sqe* sqe)
{
    spdlog::trace("handle submit ctx={}", m_context->GetName());

    m_timedOut = false;
    m_pendingCqes = 1;
    m_ring->m_pendingOps++;
    m_coord->TryAcquire(m_context);
    if (m_descriptor)
    {
        m_descriptor->m_handles.Push(this);
    }

    if (m_descriptor && m_descriptor->m_registeredIndex >= 0)
    {
        sqe->fd = m_descriptor->m_registeredIndex;
        sqe->flags |= IOSQE_FIXED_FILE;
    }

    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(this));
}

void Handle::SubmitWithTimeout(struct io_uring_sqe* sqe, time::Interval timeout)
{
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    m_timeout.tv_sec = secs.count();
    m_timeout.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(
        timeout - secs).count();
    SubmitLinked(sqe);
}

void Handle::SubmitLinked(struct io_uring_sqe* sqe)
{
    spdlog::trace("handle submit_linked ctx={}", m_context->GetName());

    m_timedOut = false;
    m_pendingCqes = 2;
    m_ring->m_pendingOps++;
    m_coord->TryAcquire(m_context);
    if (m_descriptor)
    {
        m_descriptor->m_handles.Push(this);
    }

    if (m_descriptor && m_descriptor->m_registeredIndex >= 0)
    {
        sqe->fd = m_descriptor->m_registeredIndex;
        sqe->flags |= IOSQE_FIXED_FILE;
    }

    // Mark the operation SQE as linked so the next SQE becomes a linked timeout
    //
    sqe->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(this));

    // Second SQE: linked timeout with tagged pointer (bit 0 set)
    //
    auto* timeout_sqe = m_ring->GetSqe();
    assert(timeout_sqe);
    io_uring_prep_link_timeout(timeout_sqe, &m_timeout, 0);
    io_uring_sqe_set_data(timeout_sqe, reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(this) | 1));
}

void Handle::Cancel()
{
    if (m_pendingCqes == 0)
    {
        return;
    }

    auto* sqe = m_ring->GetSqe();
    assert(sqe);

    // Cancel targets the original SQE by its userdata (untagged `this`). The cancel SQE's own
    // userdata is tagged so its completion routes to OnSecondaryComplete.
    //
    io_uring_prep_cancel(sqe, reinterpret_cast<void*>(this), 0);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(this) | 1));
    m_pendingCqes++;
}

int Handle::Wait()
{
    auto result = CoordinateWithKill(m_context, m_coord);
    if (result.Killed())
    {
        return -ECANCELED;
    }
    return m_result;
}

int Handle::Result() const
{
    assert(m_pendingCqes == 0);
    return m_result;
}

void Handle::Finalize()
{
    if (--m_pendingCqes > 0)
    {
        return;
    }

    m_ring->m_pendingOps--;

    if (m_descriptor)
    {
        this->Pop();
    }
    m_coord->Release(m_context, false /* schedule */);
}

void Handle::Complete(struct io_uring_cqe* cqe)
{
    m_result = cqe->res;
    spdlog::trace("handle complete result={}", m_result);
    io_uring_cqe_seen(&m_ring->m_ring, cqe);
    Finalize();
}

void Handle::OnSecondaryComplete(struct io_uring_cqe* cqe)
{
    spdlog::trace("handle secondary complete result={}", cqe->res);
    if (cqe->res == -ETIME)
    {
        m_timedOut = true;
    }
    io_uring_cqe_seen(&m_ring->m_ring, cqe);
    Finalize();
}

void Handle::Callback(struct io_uring_cqe* cqe)
{
    auto data = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));
    auto* handle = reinterpret_cast<Handle*>(data & ~uintptr_t(0x7));

    if (data & 1)
    {
        handle->OnSecondaryComplete(cqe);
    }
    else
    {
        handle->Complete(cqe);
    }
}

} // end namespace coop::io
} // end namespace coop
