#include <cassert>
#include <cerrno>
#include <cstdint>
#include <liburing.h>
#include <spdlog/spdlog.h>

#include "handle.h"

#include "armed_handle.h"
#include "descriptor.h"
#include "uring.h"

#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"
#include "coop/detail/timer_tag.h"
#include "coop/perf/probe.h"
#include "coop/cooperator.h"

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
    SPDLOG_TRACE("handle submit ctx={}", m_context->GetName());

    COOP_PERF_INC(m_context->GetCooperator()->GetPerfCounters(), perf::Counter::IoSubmit);
    ++m_context->m_statistics.ioSubmits;
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
    SPDLOG_TRACE("handle submit_linked ctx={}", m_context->GetName());

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

// Minimum in-flight io_uring operations before a fast-path-armed op defers its submit. Below this
// the flow is effectively serial and benefits from a prompt eager submit; above it there is enough
// concurrency that folding the submit into the batch-boundary Poll() amortizes the io_uring_enter().
//
static constexpr int kDeferMinPendingOps = 8;

bool Handle::DeferSubmit() const
{
    // Defer the eager submit only for a fast-path-armed op when enough IO is in flight that the
    // deferred io_uring_enter() has a batch to amortize into. The flag guarantees the inline
    // completion peek would be wasted (a MSG_DONTWAIT syscall already returned EAGAIN); the
    // in-flight-ops threshold guarantees the deferral pays for itself.
    //
    // A serial flow keeps only a handful of ops in flight (its own pending recv, the partner's,
    // the cross-thread submission drainer) and gains nothing from waiting for the batch boundary
    // — it instead pays an extra scheduler traversal of latency before its sole SQE reaches the
    // kernel. A concurrent server keeps many ops in flight, so the batch boundary flushes a real
    // batch in one enter.
    //
    return m_deferEagerSubmit && m_ring->PendingOps() > kDeferMinPendingOps;
}

int Handle::Wait()
{
    // Fast path: submit pending SQEs and check for immediate completion. With COOP_TASKRUN
    // (or bare mode), task_work runs during io_uring_submit(), so synchronously completed
    // operations have CQEs ready immediately. This avoids two context switches (block +
    // resume) for the common case where the kernel completes the operation inline — e.g.
    // sends to sockets with available buffer space, recvs from sockets with data already
    // queued.
    //
    // Safety: Poll() may process CQEs for other handles, moving their blocked contexts to
    // the yielded list. This is safe — cooperative scheduling means no context switch occurs
    // until we explicitly block or yield.
    //
    // Fast-path-armed ops can skip the eager submit (see MarkFastPathArmed): a MSG_DONTWAIT
    // syscall already returned EAGAIN, so there is no inline completion to catch and the eager
    // submit would only cost a per-op io_uring_enter(). The SQE then accumulates and flushes with
    // the rest of the resume batch at the scheduler's batch-boundary Poll(). DeferSubmit() gates
    // this on in-flight concurrency so a lone flow keeps its prompt eager submit.
    //
    if (!DeferSubmit())
    {
        m_ring->Poll();
        if (m_pendingCqes == 0)
        {
            return m_result;
        }
    }

    // Slow path: operation not yet complete, block until CQE arrives. The coordinator is
    // still held (from Submit's TryAcquire), so Acquire will block. When the CQE eventually
    // completes, Finalize releases the coordinator and unblocks us.
    //
    CoordinateWith(m_context, m_coord);
    return m_result;
}

int Handle::WaitKill()
{
    // Same fast path as Wait(): submit pending SQEs and consume any CQEs that are already ready.
    // A fast-path-armed op with a resume batch to amortize into defers the eager submit to the
    // batch boundary (see MarkFastPathArmed / DeferSubmit).
    //
    if (!DeferSubmit())
    {
        m_ring->Poll();
        if (m_pendingCqes == 0)
        {
            return m_result;
        }
    }

    auto result = CoordinateWithKill(m_context, m_coord);
    if (result.Killed())
    {
        return -ECANCELED;
    }

    m_coord->Release(m_context, false);
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

// Both completion handlers read cqe->res and then drop the CQE; neither depends on the kernel
// reclaiming the slot mid-drain. CQ-head advancement is therefore not performed here — Uring::Poll
// reaps a whole batch with a single io_uring_cq_advance(n) after the last callback returns,
// collapsing what would be N per-CQE release stores to the kernel-visible head into one store.
//
void Handle::Complete(struct io_uring_cqe* cqe)
{
    COOP_PERF_INC(m_context->GetCooperator()->GetPerfCounters(), perf::Counter::IoComplete);
    ++m_context->m_statistics.ioCompletes;
    m_result = cqe->res;
    SPDLOG_TRACE("handle complete result={}", m_result);
    Finalize();
}

void Handle::OnSecondaryComplete(struct io_uring_cqe* cqe)
{
    SPDLOG_TRACE("handle secondary complete result={}", cqe->res);
    if (cqe->res == -ETIME)
    {
        m_timedOut = true;
    }
    Finalize();
}

void Handle::Callback(struct io_uring_cqe* cqe)
{
    auto data = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));

    // Bit 1 (0x2) marks a multishot ArmedHandle CQE -- a distinct lifecycle that holds its
    // coordinator across an unbounded CQE stream rather than the one-shot count-to-zero this
    // Handle owns. Route it out before the one-shot decode. Bit 0 (0x1) then disambiguates within
    // each species: linked-timeout/cancel for Handle, recv-vs-cancel-ack for ArmedHandle.
    //
    if (data & 0x2)
    {
        ArmedHandle::Dispatch(cqe, data);
        return;
    }

    // Bit 2 marks the cooperator's single deadline timer (docs/timer_wheel_001.md). The expiry
    // clears the armed flag so the scheduler re-arms for the next nearest deadline; an
    // IORING_TIMEOUT_UPDATE acknowledgement (bit 0 also set) carries no information and is dropped.
    // The sleeps themselves are serviced by the scheduler loop, not here.
    //
    if (data & coop::detail::kTimerTag)
    {
        if (!(data & 0x1))
        {
            Cooperator::thread_cooperator->OnTimerExpired();
        }
        return;
    }

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
