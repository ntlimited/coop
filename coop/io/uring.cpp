#include "uring.h"

#include <spdlog/spdlog.h>

#include "handle.h"

#include "coop/context.h"

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

    int ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    if (ret == -EINVAL && (flags & IORING_SETUP_DEFER_TASKRUN))
    {
        // DEFER_TASKRUN requires kernel 6.1+. Fall back to COOP_TASKRUN.
        //
        flags &= ~IORING_SETUP_DEFER_TASKRUN;
        flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
        spdlog::warn("uring init: DEFER_TASKRUN not supported, falling back to COOP_TASKRUN");
        memset(&params, 0, sizeof(params));
        params.flags = flags;
        ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    }
    if (ret == -EINVAL && (flags & IORING_SETUP_SINGLE_ISSUER))
    {
        // SINGLE_ISSUER requires kernel 6.0+. Retry without it but keep user-requested flags.
        //
        flags &= ~IORING_SETUP_SINGLE_ISSUER;
        spdlog::warn("uring init: SINGLE_ISSUER not supported, retrying with flags {:#x}", flags);
        memset(&params, 0, sizeof(params));
        params.flags = flags;
        ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    }
    if (ret == -EINVAL && flags != 0)
    {
        spdlog::warn("uring init with flags {:#x} failed, retrying without", flags);
        memset(&params, 0, sizeof(params));
        ret = io_uring_queue_init_params(m_config.entries, &m_ring, &params);
    }
    assert(ret == 0);
    std::ignore = ret;

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
        spdlog::trace("uring cqe result={}", cqe->res);
        Handle::Callback(cqe);
        dispatched++;
    }

    return dispatched;
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
            spdlog::trace("uring register fd={} slot={}", descriptor->m_fd, i);
            return;
        }
    }
    spdlog::debug("uring register fd={} no slot available", descriptor->m_fd);
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
    spdlog::trace("uring unregister fd={} slot={}", descriptor->m_fd, idx);
}

} // end namespace io
} // end namespace coop
