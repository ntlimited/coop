#include "uring.h"

#include <cstdlib>
#include <unistd.h>
#include <spdlog/spdlog.h>

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

    if (!m_registered.empty())
    {
        ret = io_uring_register_files(&m_ring, m_registered.data(), m_registered.size());
        if (ret < 0)
        {
            spdlog::warn("uring register_files failed ret={}", ret);
            m_registered.clear();
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

    int dispatched = 0;
    struct io_uring_cqe* cqe;

    while (io_uring_peek_cqe(&m_ring, &cqe) == 0)
    {
        SPDLOG_TRACE("uring cqe result={}", cqe->res);

        COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(), perf::Counter::PollCqe);
        Handle::Callback(cqe);
        dispatched++;
    }

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
