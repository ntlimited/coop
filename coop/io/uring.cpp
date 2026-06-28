#include "uring.h"

#include <cstdlib>
#include <unistd.h>
#include <spdlog/spdlog.h>

#include "buffer_ring.h"
#include "handle.h"

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/perf/probe.h"

// Compat defines for kernels/liburing that don't expose these yet. These are stable kernel ABI.
//
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif

#ifndef IORING_SQ_TASKRUN
#define IORING_SQ_TASKRUN (1U << 0)
// liburing < 2.3 lacks io_uring_get_events(). Polyfill via raw syscall: zero-submit enter
// with IORING_ENTER_GETEVENTS to flush deferred completions.
//
#include <sys/syscall.h>
#include <unistd.h>
static inline int io_uring_get_events(struct io_uring* ring)
{
    return syscall(__NR_io_uring_enter, ring->ring_fd, 0, 0,
        IORING_ENTER_GETEVENTS, nullptr, _NSIG / 8);
}
#endif

namespace coop
{

namespace io
{

// Constructor and destructor live here, where BufferRing is complete, so the unique_ptr member
// can be cleanly cleaned up (constructor exception path) and torn down (destructor unregister).
//
Uring::Uring(UringConfiguration const& config)
: m_config(config)
, m_registered(config.registeredSlots, -1)
{
    memset(&m_ring, 0, sizeof(m_ring));
}

Uring::~Uring() = default;

void Uring::Init()
{
    unsigned flags = IORING_SETUP_SINGLE_ISSUER;
    if (m_config.sqpoll)
    {
        flags |= IORING_SETUP_SQPOLL;
    }
    if (m_config.iopoll)
    {
        flags |= IORING_SETUP_IOPOLL;
    }
    if (m_config.deferTaskrun)
    {
        // DEFER_TASKRUN supersedes COOP_TASKRUN — completions are only processed when
        // explicitly requested via io_uring_get_events(). Requires SINGLE_ISSUER (always
        // forced) and kernel 6.1+.
        //
        flags |= IORING_SETUP_DEFER_TASKRUN;
    }
    else if (m_config.coopTaskrun)
    {
        flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
    }

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = flags;
    if (m_config.sqpoll && m_config.sqpollIdleMs > 0)
    {
        params.sq_thread_idle = m_config.sqpollIdleMs;
    }
    if (m_config.attachSqFd >= 0)
    {
        flags |= IORING_SETUP_ATTACH_WQ;
        params.flags = flags;
        params.wq_fd = m_config.attachSqFd;
    }

    int ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    if (ret < 0 && (flags & IORING_SETUP_DEFER_TASKRUN))
    {
        // DEFER_TASKRUN requires kernel 6.1+. Fall back to COOP_TASKRUN.
        //
        flags &= ~IORING_SETUP_DEFER_TASKRUN;
        flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
        spdlog::warn("uring init: DEFER_TASKRUN failed ret={}, falling back to COOP_TASKRUN", ret);
        memset(&params, 0, sizeof(params));
        params.flags = flags;
        ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    }
    if (ret < 0 && (flags & IORING_SETUP_SINGLE_ISSUER))
    {
        // Retry without SINGLE_ISSUER but keep user-requested taskrun/defer flags.
        //
        flags &= ~IORING_SETUP_SINGLE_ISSUER;
        spdlog::warn("uring init: SINGLE_ISSUER path failed ret={}, retrying with flags {:#x}",
            ret, flags);
        memset(&params, 0, sizeof(params));
        params.flags = flags;
        ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    }
    if (ret < 0 && flags != 0)
    {
        spdlog::warn("uring init with flags {:#x} failed ret={}, retrying without flags",
            flags, ret);
        memset(&params, 0, sizeof(params));
        ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    }
    if (ret != 0)
    {
        // Fail fast in all build modes. Continuing with an uninitialized ring
        // turns into null dereferences later (for example in GetSqe()).
        //
        spdlog::critical("uring init failed ret={} requested_flags={:#x}", ret, flags);
        std::abort();
    }
    assert(ret == 0);

    spdlog::info("uring init flags={:#x}", m_ring.flags);

    // Register this ring's fd into the calling thread's registered-ring table so that subsequent
    // io_uring_enter() calls reference the ring by a small registered index rather than by file
    // descriptor (IORING_ENTER_REGISTERED_RING). io_uring_enter() is the hottest syscall coop
    // makes — every submit-then-wait round trip pays for it — and coop's deployment is exactly the
    // case the kernel penalizes: many cooperators in one process, one ring per thread, so the
    // process file table is shared and each enter pays an atomic fd refcount grab/put plus the
    // fd->file lookup. A registered ring skips that lookup, saving ~15ns per enter on this hardware.
    //
    // liburing flips the ring into registered-fd mode on success (it records the registered index
    // and ORs IORING_ENTER_REGISTERED_RING into every later enter automatically), so the
    // Submit/Poll/WaitAndPoll paths need no change. The call is safe under the one documented
    // caveat — a ring registered by one thread but entered by another misbehaves — because
    // SINGLE_ISSUER already guarantees only the owning cooperator thread enters this ring, and
    // Init() runs on that thread.
    //
    // IORING_REGISTER_RING_FDS needs kernel 5.18+. On older kernels the register call fails and the
    // ring keeps using its plain fd — correct, just without the saving — so warn and continue
    // rather than abort.
    //
    ret = io_uring_register_ring_fd(&m_ring);
    if (ret < 0)
    {
        spdlog::warn("uring register_ring_fd failed ret={}, using unregistered enter path", ret);
    }

    if (!m_registered.empty())
    {
        ret = io_uring_register_files(&m_ring, m_registered.data(), m_registered.size());
        if (ret < 0)
        {
            spdlog::warn("uring register_files failed ret={}", ret);
            m_registered.clear();
        }
    }

    // Register the optional default provided buffer ring. The registration doubles as the runtime
    // feature probe: on a kernel without pbuf-ring support (pre-5.19) io_uring_setup_buf_ring
    // fails, and we warn and continue with no default ring -- classic recv is untouched -- exactly
    // as the registered-ring-fd path above degrades. Entries must be a power of two; round up.
    //
    if (m_config.bufferRingEntries > 0)
    {
        uint32_t entries = 1;
        while (entries < m_config.bufferRingEntries)
        {
            entries <<= 1;
        }
        auto ring = std::make_unique<BufferRing>(
            m_config.bufferRingGroup, entries, m_config.bufferRingBufSize);
        int err = ring->Register(*this);
        if (err < 0)
        {
            spdlog::warn("uring buffer-ring register failed ret={}, using classic recv", err);
        }
        else
        {
            spdlog::info("uring buffer-ring registered group={} entries={} bufSize={}",
                m_config.bufferRingGroup, entries, m_config.bufferRingBufSize);
            m_bufferRing = std::move(ring);
        }
    }
}

int Uring::Submit()
{
    if (m_pendingSqes <= 0)
    {
        return 0;
    }

    int submitted = io_uring_submit(&m_ring);
    m_pendingSqes = 0;
    return submitted;
}

struct io_uring_sqe* Uring::GetSqe()
{
    if (!m_ring.sq.khead)
    {
        spdlog::critical("GetSqe called before successful uring init");
        std::abort();
    }

    auto* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
    {
        // SQ ring is full — flush pending SQEs and retry
        //
        io_uring_submit(&m_ring);
        m_pendingSqes = 0;
        sqe = io_uring_get_sqe(&m_ring);
    }
    if (sqe)
    {
        m_pendingSqes++;
    }
    return sqe;
}

bool Uring::HasPendingCompletions() const
{
    // Two independent signals that real IO is ready to service. The continuation drain consults
    // this to decide whether to yield mid-chain.
    //
    //   1. io_uring_cq_ready: CQEs already materialized in the completion ring, waiting for Poll
    //      to dispatch them. A userspace read of the CQ head/tail.
    //   2. IORING_SQ_TASKRUN: under COOP_TASKRUN the kernel completes operations into task_work
    //      and sets this flag rather than filling the CQ ring directly; the CQEs only appear after
    //      the next io_uring_enter(). During a synchronous continuation chain no enter() happens,
    //      so cq_ready stays zero even though completions are pending -- this flag is the live
    //      signal for that case. A volatile read of kernel-mapped SQ ring memory.
    //
    return io_uring_cq_ready(&m_ring) > 0 || (*m_ring.sq.kflags & IORING_SQ_TASKRUN);
}

int Uring::Poll()
{
    // Check whether io_uring_submit() is needed before calling it. Poll() is invoked after
    // every context Resume() in the scheduler loop, and most resumes don't produce SQEs (pure
    // yields, coordinator-only operations). Checking here avoids io_uring_submit()'s internal
    // bookkeeping (__io_uring_flush_sq + sq_ring_needs_enter) on the nothing-to-do path —
    // roughly 5-10ns per call, ~2-3% of the yield hot path.
    //
    COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(), perf::Counter::PollCycle);

    // Two conditions require a submit:
    //   1. m_pendingSqes > 0: new SQEs from Handle::Submit/SubmitLinked/Cancel need flushing.
    //      io_uring_enter() also runs task_work as a side effect, so this covers both.
    //   2. IORING_SQ_TASKRUN flag set (COOP_TASKRUN mode only): the kernel completed an
    //      operation and has pending task_work to deliver. Without flushing, CQEs won't appear
    //      in the CQ ring and blocked contexts would never wake. The flag is a volatile read
    //      from kernel-mapped memory (sq.kflags); SINGLE_ISSUER keeps it uncontended.
    //
    // Note: liburing also checks CQ overflow inside sq_ring_needs_enter(). We skip that here;
    // CQ overflow is pathological (means CQEs aren't being drained fast enough) and the next
    // real submit catches it. Not worth an extra branch on every Poll().
    //
    // For DEFER_TASKRUN: io_uring_submit() cannot flush deferred completions (it doesn't pass
    // IORING_ENTER_GETEVENTS when submitted==0). io_uring_get_events() is required separately.
    //
    if (m_pendingSqes > 0 || (*m_ring.sq.kflags & IORING_SQ_TASKRUN))
    {
        COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(), perf::Counter::PollSubmit);
        io_uring_submit(&m_ring);
        m_pendingSqes = 0;
    }

    if (m_ring.flags & IORING_SETUP_DEFER_TASKRUN)
    {
        io_uring_get_events(&m_ring);
    }

    // Reap the whole ready batch and advance the CQ head once. Each callback only reads cqe->res
    // (see Handle::Complete) and never relies on the kernel reclaiming a slot mid-drain, so the
    // per-CQE io_uring_cqe_seen the callbacks used to issue — a release store to the kernel-visible
    // head per completion — collapses into a single io_uring_cq_advance(dispatched) here. On a
    // fan-out burst this turns N head stores into one; on the common single-CQE Poll it is exactly
    // one store either way, so there is no regression on the hot path.
    //
    int dispatched = 0;
    struct io_uring_cqe* cqe;
    unsigned head;

    io_uring_for_each_cqe(&m_ring, head, cqe)
    {
        SPDLOG_TRACE("uring cqe result={}", cqe->res);

        COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(), perf::Counter::PollCqe);
        Handle::Callback(cqe);
        dispatched++;
    }

    io_uring_cq_advance(&m_ring, dispatched);

    return dispatched;
}

int Uring::ReapOnly()
{
    COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(), perf::Counter::PollCycle);

    // Dispatch the completions a prior submit already materialized, without entering the kernel.
    // As in Poll(), the callbacks only read cqe->res and never advance the CQ head themselves, so
    // the whole ready batch is reaped with io_uring_for_each_cqe and the head is moved once with a
    // single io_uring_cq_advance(dispatched). Peeking without that advance would re-dispatch the
    // same CQE forever.
    //
    int dispatched = 0;
    struct io_uring_cqe* cqe;
    unsigned head;

    io_uring_for_each_cqe(&m_ring, head, cqe)
    {
        COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(), perf::Counter::PollCqe);
        Handle::Callback(cqe);
        dispatched++;
    }

    io_uring_cq_advance(&m_ring, dispatched);

    return dispatched;
}

int Uring::WaitAndPoll()
{
    // Flush any pending SQEs before blocking — they may produce the CQE we're about to wait for.
    //
    if (m_pendingSqes > 0)
    {
        io_uring_submit(&m_ring);
        m_pendingSqes = 0;
    }

    // Block until at least one CQE is available. With COOP_TASKRUN, this also processes any
    // pending task_work (deferred completions).
    //
    struct io_uring_cqe* cqe;
    int ret = io_uring_wait_cqe(&m_ring, &cqe);
    if (ret < 0)
    {
        return 0;
    }

    // Process all available CQEs (the wait may have delivered multiple).
    //
    return Poll();
}


// Work loop for non-native urings that run as a dedicated context. Poll() handles both
// submitting pending SQEs and processing CQEs, so the yield-poll loop naturally batches
// SQEs filled by other contexts between scheduling rounds.
//
void Uring::Run(Context* ctx)
{
    ctx->SetName(m_config.taskName);

    Init();

    while (!ctx->IsKilled())
    {
        ctx->Yield();
        Poll();
    }
}

void Uring::Register(Descriptor* descriptor)
{
    int slots = static_cast<int>(m_registered.size());
    for (int i = 0; i < slots; i++)
    {
        if (m_registered[i] == -1)
        {
            m_registered[i] = descriptor->m_fd;
            int ret = io_uring_register_files_update(&m_ring, i, &descriptor->m_fd, 1);
            if (ret < 0)
            {
                spdlog::warn("uring register_files_update failed fd={} ret={}",
                    descriptor->m_fd, ret);
                m_registered[i] = -1;
                return;
            }
            descriptor->m_registeredIndex = i;
            SPDLOG_TRACE("uring register fd={} slot={}", descriptor->m_fd, i);
            return;
        }
    }
    SPDLOG_DEBUG("uring register fd={} no slot available", descriptor->m_fd);
}

void Uring::Unregister(Descriptor* descriptor)
{
    int idx = descriptor->m_registeredIndex;
    assert(idx >= 0 && idx < static_cast<int>(m_registered.size()));

    int negOne = -1;
    m_registered[idx] = -1;
    int ret = io_uring_register_files_update(&m_ring, idx, &negOne, 1);
    if (ret < 0)
    {
        spdlog::warn("uring unregister_files_update failed fd={} slot={} ret={}",
            descriptor->m_fd, idx, ret);
    }
    descriptor->m_registeredIndex = -1;
    SPDLOG_TRACE("uring unregister fd={} slot={}", descriptor->m_fd, idx);
}

} // end namespace io
} // end namespace coop
