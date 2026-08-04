#include "uring.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
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

static inline pid_t GetTid()
{
#ifdef SYS_gettid
    return static_cast<pid_t>(syscall(SYS_gettid));
#else
    return gettid();
#endif
}

#ifndef NDEBUG
static int s_injectedEnterError{0};
void Uring::SetInjectedEnterError(int err)
{
    s_injectedEnterError = err;
}

static std::atomic<uint64_t> s_offOwnerThreadTeardownCount{0};
uint64_t Uring::OffOwnerThreadTeardownCount()
{
    return s_offOwnerThreadTeardownCount.load(std::memory_order_relaxed);
}
void Uring::ResetOffOwnerThreadTeardownCount()
{
    s_offOwnerThreadTeardownCount.store(0, std::memory_order_relaxed);
}
#endif

static bool UringLedgerEnabled()
{
    static bool enabled = []() -> bool
    {
        char const* env = getenv("COOP_URING_LEDGER");
        return env && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0');
    }();
    return enabled;
}

static void UringLedgerPrintf(char const* fmt, ...)
{
    if (!UringLedgerEnabled())
    {
        return;
    }

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    spdlog::info("{}", buf);
}

static void UringLedgerSqe(char const* event,
                           char const* site,
                           Uring const* ring,
                           struct io_uring_sqe const* sqe)
{
    if (!UringLedgerEnabled())
    {
        return;
    }

    UringLedgerPrintf(
        "COOP_URING_LEDGER event=%s site=%s ring=%p sqe=%p opcode=%u flags=%u "
        "fd=%d off=%llu addr=%llu len=%u user_data=%#llx pending_sqes=%d "
        "pending_ops=%d acquired_since_submit=%d accounted_since_submit=%d",
        event,
        site,
        static_cast<void const*>(ring),
        static_cast<void const*>(sqe),
        sqe ? static_cast<unsigned>(sqe->opcode) : 0u,
        sqe ? static_cast<unsigned>(sqe->flags) : 0u,
        sqe ? sqe->fd : -1,
        sqe ? static_cast<unsigned long long>(sqe->off) : 0ull,
        sqe ? static_cast<unsigned long long>(sqe->addr) : 0ull,
        sqe ? sqe->len : 0u,
        sqe ? static_cast<unsigned long long>(sqe->user_data) : 0ull,
        ring->m_pendingSqes,
        ring->m_pendingOps,
        ring->m_ledgerAcquiredSinceSubmit,
        ring->m_ledgerAccountedSinceSubmit);
}

static void UringLedgerSubmit(char const* site,
                              Uring const* ring,
                              int pendingSqesBefore,
                              int acquiredBefore,
                              int accountedBefore,
                              int submitted)
{
    UringLedgerPrintf(
        "COOP_URING_LEDGER event=KERNEL_ENTERED site=%s ring=%p "
        "pending_sqes_before=%d acquired_since_submit=%d "
        "accounted_since_submit=%d submitted=%d pending_ops=%d",
        site,
        static_cast<void const*>(ring),
        pendingSqesBefore,
        acquiredBefore,
        accountedBefore,
        submitted,
        ring->m_pendingOps);
}

static void UringLedgerResetSubmitWindow(Uring* ring)
{
    ring->m_ledgerAcquiredSinceSubmit = 0;
    ring->m_ledgerAccountedSinceSubmit = 0;
}

// Constructor and destructor live here, where BufferRing is complete, so the unique_ptr member
// can be cleanly cleaned up (constructor exception path) and torn down (destructor unregister).
//
Uring::Uring(UringConfiguration const& config)
: m_registered(config.registeredSlots, -1)
, m_config(config)
{
    memset(&m_ring, 0, sizeof(m_ring));
}

Uring::~Uring()
{
    Teardown();
}

void Uring::Teardown()
{
    if (!m_initialized)
    {
        return;
    }

    pid_t tid = GetTid();
    unsigned int intFlags = static_cast<unsigned int>(m_ring.int_flags);
    bool liveReg = (intFlags & 1u) != 0;

    LedgerTeardown("Teardown");

    if (liveReg && m_ownerTid != 0 && m_ownerTid != tid)
    {
        assert(false && "Uring::Teardown called off owner thread with live registered ring");
        spdlog::critical(
            "uring teardown on foreign thread ring={} owner_tid={} current_tid={} - skipping unregister",
            static_cast<void const*>(this),
            m_ownerTid,
            tid);
        return;
    }

    m_bufferRing.reset();
    if (m_filesRegistered)
    {
        (void)io_uring_unregister_files(&m_ring);
        m_filesRegistered = false;
    }
    io_uring_queue_exit(&m_ring);
    m_initialized = false;
}

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
    m_initialized = true;
    m_ownerTid = GetTid();

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
        // Sparse registration: the kernel allocates an empty fixed-file table and slots
        // attach later through FILES_UPDATE — no full-table upload at Init. Pre-check
        // RLIMIT_NOFILE headroom ourselves: liburing's helper reacts to -EMFILE by
        // silently setrlimit()ing the PROCESS limit and retrying, and a per-ring library
        // call must not mutate process-global state (coop is N rings in one process).
        //
        rlimit nofile{};
        if (getrlimit(RLIMIT_NOFILE, &nofile) == 0 &&
            m_registered.size() > nofile.rlim_cur)
        {
            spdlog::warn("uring registeredSlots={} exceeds RLIMIT_NOFILE={}; "
                         "fd registration disabled (raise the rlimit at startup)",
                         m_registered.size(), nofile.rlim_cur);
        }
        else
        {
            ret = io_uring_register_files_sparse(&m_ring, m_registered.size());
            if (ret < 0)
            {
                spdlog::warn("uring register_files_sparse failed ret={}", ret);
                m_registered.clear();
            }
            else
            {
                m_filesRegistered = true;
                m_freeSlots.reserve(m_registered.size());
                for (int i = static_cast<int>(m_registered.size()) - 1; i >= 0; i--)
                {
                    m_freeSlots.push_back(i);
                }
            }
        }
    }

    // Optional io-wq worker caps. The bounded pool (buffered file IO that punts) defaults
    // to min(sq_entries, 4 * cpus) PER RING, and coop runs one ring per cooperator — an
    // explicit cap bounds the process-wide aggregate. Warn-and-continue on kernels
    // without IORING_REGISTER_IOWQ_MAX_WORKERS (pre-5.15), like every probe above.
    //
    if (m_config.iowqMaxBoundedWorkers > 0 || m_config.iowqMaxUnboundedWorkers > 0)
    {
        unsigned int caps[2] = { m_config.iowqMaxBoundedWorkers,
                                 m_config.iowqMaxUnboundedWorkers };
        ret = io_uring_register_iowq_max_workers(&m_ring, caps);
        if (ret < 0)
        {
            spdlog::warn("uring register_iowq_max_workers failed ret={}", ret);
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
    assert(m_initialized && "Submit called on uninitialized or torn-down ring");
    if (m_pendingSqes <= 0)
    {
        return 0;
    }

    int pendingSqesBefore = m_pendingSqes;
    int acquiredBefore = m_ledgerAcquiredSinceSubmit;
    int accountedBefore = m_ledgerAccountedSinceSubmit;
#ifndef NDEBUG
    int submitted;
    if (s_injectedEnterError != 0)
    {
        submitted = s_injectedEnterError;
        s_injectedEnterError = 0;
    }
    else
    {
        submitted = io_uring_submit(&m_ring);
    }
#else
    int submitted = io_uring_submit(&m_ring);
#endif
    UringLedgerSubmit("Uring::Submit",
                      this,
                      pendingSqesBefore,
                      acquiredBefore,
                      accountedBefore,
                      submitted);

    if (submitted >= 0)
    {
        m_pendingSqes = (submitted >= m_pendingSqes) ? 0 : (m_pendingSqes - submitted);
        UringLedgerResetSubmitWindow(this);
        return submitted;
    }

    LedgerEnterFailure("Uring::Submit", submitted);
    if (IsRetryableError(submitted))
    {
        return 0;
    }

    HandleFatalEnterError("Uring::Submit", submitted);
    return 0;
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
        int pendingSqesBefore = m_pendingSqes;
        int acquiredBefore = m_ledgerAcquiredSinceSubmit;
        int accountedBefore = m_ledgerAccountedSinceSubmit;
#ifndef NDEBUG
        int submitted;
        if (s_injectedEnterError != 0)
        {
            submitted = s_injectedEnterError;
            s_injectedEnterError = 0;
        }
        else
        {
            submitted = io_uring_submit(&m_ring);
        }
#else
        int submitted = io_uring_submit(&m_ring);
#endif
        UringLedgerSubmit("Uring::GetSqe.retry",
                          this,
                          pendingSqesBefore,
                          acquiredBefore,
                          accountedBefore,
                          submitted);
        if (submitted >= 0)
        {
            m_pendingSqes = (submitted >= m_pendingSqes) ? 0 : (m_pendingSqes - submitted);
            UringLedgerResetSubmitWindow(this);
        }
        else
        {
            LedgerEnterFailure("Uring::GetSqe.retry", submitted);
            if (!IsRetryableError(submitted))
            {
                HandleFatalEnterError("Uring::GetSqe.retry", submitted);
            }
        }
        sqe = io_uring_get_sqe(&m_ring);
    }
    if (sqe)
    {
        m_pendingSqes++;
        if (UringLedgerEnabled())
        {
            m_ledgerAcquiredSinceSubmit++;
        }
        UringLedgerSqe("SQE_ACQUIRED", "Uring::GetSqe", this, sqe);
    }
    return sqe;
}

void Uring::LedgerAccounted(char const* site,
                            struct io_uring_sqe const* sqe,
                            uintptr_t userData)
{
    if (!UringLedgerEnabled())
    {
        return;
    }

    m_ledgerAccountedSinceSubmit++;
    UringLedgerPrintf(
        "COOP_URING_LEDGER event=ACCOUNTED site=%s ring=%p sqe=%p "
        "user_data=%#llx opcode=%u flags=%u fd=%d off=%llu addr=%llu len=%u "
        "pending_sqes=%d pending_ops=%d acquired_since_submit=%d "
        "accounted_since_submit=%d",
        site,
        static_cast<void const*>(this),
        static_cast<void const*>(sqe),
        static_cast<unsigned long long>(userData),
        sqe ? static_cast<unsigned>(sqe->opcode) : 0u,
        sqe ? static_cast<unsigned>(sqe->flags) : 0u,
        sqe ? sqe->fd : -1,
        sqe ? static_cast<unsigned long long>(sqe->off) : 0ull,
        sqe ? static_cast<unsigned long long>(sqe->addr) : 0ull,
        sqe ? sqe->len : 0u,
        m_pendingSqes,
        m_pendingOps,
        m_ledgerAcquiredSinceSubmit,
        m_ledgerAccountedSinceSubmit);
}

void Uring::LedgerCompleted(char const* site,
                            uintptr_t userData,
                            int result,
                            int pendingCqesBefore,
                            int pendingCqesAfter,
                            int pendingOpsBefore,
                            int pendingOpsAfter)
{
    UringLedgerPrintf(
        "COOP_URING_LEDGER event=COMPLETED site=%s ring=%p user_data=%#llx "
        "result=%d pending_cqes_before=%d pending_cqes_after=%d "
        "pending_ops_before=%d pending_ops_after=%d",
        site,
        static_cast<void const*>(this),
        static_cast<unsigned long long>(userData),
        result,
        pendingCqesBefore,
        pendingCqesAfter,
        pendingOpsBefore,
        pendingOpsAfter);
}

void Uring::LedgerEnterFailure(char const* site, int err)
{
    pid_t tid = GetTid();
    int ringFd = m_ring.ring_fd;
    int enterFd = m_ring.enter_ring_fd;
    unsigned int intFlags = static_cast<unsigned int>(m_ring.int_flags);

    int plainRetry = -1;
    if (ringFd >= 0)
    {
        plainRetry = static_cast<int>(syscall(__NR_io_uring_enter, ringFd, 0, 0, 0, nullptr, 0));
        if (plainRetry < 0)
        {
            plainRetry = -errno;
        }
    }

    UringLedgerPrintf(
        "COOP_URING_LEDGER event=ENTER_FAILURE site=%s ring=%p owner_tid=%d current_tid=%d "
        "ring_fd=%d enter_ring_fd=%d int_flags=%#x errno=%d plain_fd_retry=%d",
        site,
        static_cast<void const*>(this),
        m_ownerTid,
        tid,
        ringFd,
        enterFd,
        intFlags,
        err,
        plainRetry);
}

void Uring::LedgerTeardown(char const* site)
{
    pid_t tid = GetTid();
    bool initialized = m_initialized;
    int ringFd = initialized ? m_ring.ring_fd : -1;
    int enterFd = initialized ? m_ring.enter_ring_fd : -1;
    unsigned int intFlags = initialized ? static_cast<unsigned int>(m_ring.int_flags) : 0u;
    bool liveReg = initialized && ((intFlags & 1u) != 0);

    if (initialized && liveReg && m_ownerTid != 0 && m_ownerTid != tid)
    {
#ifndef NDEBUG
        s_offOwnerThreadTeardownCount.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    UringLedgerPrintf(
        "COOP_URING_LEDGER event=TEARDOWN site=%s ring=%p owner_tid=%d current_tid=%d "
        "initialized=%d ring_fd=%d enter_ring_fd=%d int_flags=%#x live_reg=%d",
        site,
        static_cast<void const*>(this),
        m_ownerTid,
        tid,
        initialized ? 1 : 0,
        ringFd,
        enterFd,
        intFlags,
        liveReg ? 1 : 0);
}

bool Uring::IsRetryableError(int err)
{
    int e = err < 0 ? -err : err;
    return e == EAGAIN || e == EINTR || e == EBUSY;
}

void Uring::HandleFatalEnterError(char const* site, int err)
{
    int errCode = err < 0 ? -err : err;
    pid_t tid = GetTid();
    spdlog::critical(
        "uring enter fatal error site={} ring={} ring_fd={} enter_ring_fd={} owner_tid={} current_tid={} errno={}: {}",
        site,
        static_cast<void const*>(this),
        m_ring.ring_fd,
        m_ring.enter_ring_fd,
        m_ownerTid,
        tid,
        errCode,
        std::strerror(errCode));
    assert(false && "Fatal io_uring enter error");
    std::abort();
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
    return io_uring_cq_ready(&m_ring) > 0
        || (IO_URING_READ_ONCE(*m_ring.sq.kflags) & IORING_SQ_TASKRUN);
}

int Uring::Poll()
{
    assert(m_initialized && "Poll called on uninitialized or torn-down ring");
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
    if (m_pendingSqes > 0 || (IO_URING_READ_ONCE(*m_ring.sq.kflags) & IORING_SQ_TASKRUN))
    {
        COOP_PERF_INC(Cooperator::thread_cooperator->GetPerfCounters(), perf::Counter::PollSubmit);
        int pendingSqesBefore = m_pendingSqes;
        int acquiredBefore = m_ledgerAcquiredSinceSubmit;
        int accountedBefore = m_ledgerAccountedSinceSubmit;
#ifndef NDEBUG
        int submitted;
        if (s_injectedEnterError != 0)
        {
            submitted = s_injectedEnterError;
            s_injectedEnterError = 0;
        }
        else
        {
            submitted = io_uring_submit(&m_ring);
        }
#else
        int submitted = io_uring_submit(&m_ring);
#endif
        UringLedgerSubmit("Uring::Poll",
                          this,
                          pendingSqesBefore,
                          acquiredBefore,
                          accountedBefore,
                          submitted);

        if (submitted >= 0)
        {
            m_pendingSqes = (submitted >= m_pendingSqes) ? 0 : (m_pendingSqes - submitted);
            UringLedgerResetSubmitWindow(this);
        }
        else
        {
            LedgerEnterFailure("Uring::Poll", submitted);
            if (!IsRetryableError(submitted))
            {
                HandleFatalEnterError("Uring::Poll", submitted);
            }
        }
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
    assert(m_initialized && "ReapOnly called on uninitialized or torn-down ring");
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
    assert(m_initialized && "WaitAndPoll called on uninitialized or torn-down ring");
    // Flush pending SQEs AND block for a completion in one io_uring_enter. The separate
    // io_uring_submit() + io_uring_wait_cqe() this replaces cost two enters per idle poll; on a busy
    // fan-out server the idle poll runs ~once per round-trip, so fusing them halves the wait-side
    // enter rate -- and the per-syscall pthread-cancel overhead that rides every enter with it. With
    // COOP_TASKRUN this enter also runs pending task_work, delivering deferred completions.
    //
    int pendingSqesBefore = m_pendingSqes;
    int acquiredBefore = m_ledgerAcquiredSinceSubmit;
    int accountedBefore = m_ledgerAccountedSinceSubmit;
#ifndef NDEBUG
    int ret;
    if (s_injectedEnterError != 0)
    {
        ret = s_injectedEnterError;
        s_injectedEnterError = 0;
    }
    else
    {
        ret = io_uring_submit_and_wait(&m_ring, 1);
    }
#else
    int ret = io_uring_submit_and_wait(&m_ring, 1);
#endif
    UringLedgerSubmit("Uring::WaitAndPoll",
                      this,
                      pendingSqesBefore,
                      acquiredBefore,
                      accountedBefore,
                      ret);

    if (ret >= 0)
    {
        m_pendingSqes = (ret >= m_pendingSqes) ? 0 : (m_pendingSqes - ret);
        UringLedgerResetSubmitWindow(this);
    }
    else
    {
        LedgerEnterFailure("Uring::WaitAndPoll", ret);
        if (IsRetryableError(ret))
        {
            return 0;
        }
        HandleFatalEnterError("Uring::WaitAndPoll", ret);
    }

    // Process all available CQEs the enter delivered (it may have reaped several).
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
    if (!m_filesRegistered || m_registrationBroken || m_freeSlots.empty())
    {
        SPDLOG_DEBUG("uring register fd={} unavailable (registered={} broken={} free={})",
            descriptor->m_fd, m_filesRegistered, m_registrationBroken, m_freeSlots.size());
        return;
    }

    int i = m_freeSlots.back();
    m_freeSlots.pop_back();

    int ret = io_uring_register_files_update(&m_ring, i, &descriptor->m_fd, 1);
    if (ret < 0)
    {
        // Sticky latch: a failing update path is a configuration problem (rlimits,
        // kernel state), and half-registered workloads are harder to reason about than
        // unregistered ones. Disable for the ring's lifetime.
        //
        spdlog::warn("uring register_files_update failed fd={} ret={}; "
                     "fd registration disabled for this ring", descriptor->m_fd, ret);
        m_freeSlots.push_back(i);
        m_registrationBroken = true;
        return;
    }

    m_registered[i] = descriptor->m_fd;
    descriptor->m_registeredIndex = i;
    SPDLOG_TRACE("uring register fd={} slot={}", descriptor->m_fd, i);
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
        // The slot may still pin the file kernel-side — do NOT recycle it (a future
        // occupant would alias), and latch the feature off.
        //
        spdlog::warn("uring unregister_files_update failed fd={} slot={} ret={}; "
                     "slot quarantined, fd registration disabled for this ring",
            descriptor->m_fd, idx, ret);
        m_registrationBroken = true;
    }
    else
    {
        m_freeSlots.push_back(idx);
    }
    descriptor->m_registeredIndex = -1;
    SPDLOG_TRACE("uring unregister fd={} slot={}", descriptor->m_fd, idx);
}

} // end namespace io
} // end namespace coop
