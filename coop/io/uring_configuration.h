#pragma once

namespace coop
{
namespace io
{

struct UringConfiguration
{
    int entries = 64;
    int registeredSlots = 64;
    const char* taskName = "Uring";

    // IORING_SETUP_SQPOLL: kernel thread polls the SQ for new entries, avoiding io_uring_enter()
    // for submission. May require CAP_SYS_ADMIN or appropriate cgroup permissions. Incompatible
    // with coopTaskrun/deferTaskrun — disable those when enabling SQPOLL.
    //
    bool sqpoll = false;

    // How long (ms) the SQPOLL kernel thread busy-polls before going idle. Only meaningful when
    // sqpoll is true. When the thread goes idle, the next submission requires an io_uring_enter()
    // with IORING_ENTER_SQ_WAKEUP to wake it. Higher values keep the thread hot between bursts
    // at the cost of CPU. 0 uses the kernel default (typically 1000ms).
    //
    unsigned sqpollIdleMs = 0;

    // IORING_SETUP_IOPOLL: kernel busy-polls for I/O completions instead of using interrupts.
    // Only works with O_DIRECT / pollable operations. NOTE: the current Uring::Run() loop uses
    // io_uring_peek_cqe which does not trigger IOPOLL reaping — enabling this requires Run()
    // adaptations that are not yet implemented.
    //
    bool iopoll = false;

    // IORING_SETUP_COOP_TASKRUN + IORING_SETUP_TASKRUN_FLAG: defers kernel task_work to the
    // next io_uring_enter() instead of async delivery. Natural fit for the cooperative model.
    // Requires kernel 5.19+ and liburing that checks IORING_SQ_TASKRUN in peek paths (≥2.3).
    //
    bool coopTaskrun = true;

    // IORING_SETUP_DEFER_TASKRUN: stronger variant of coopTaskrun. Task_work is only processed
    // when the application explicitly requests it (io_uring_get_events), giving full control
    // over when completions are processed. Requires SINGLE_ISSUER (always forced) and kernel
    // 6.1+. Supersedes coopTaskrun — when both are set, deferTaskrun takes priority and
    // coopTaskrun is used as the fallback if the kernel rejects DEFER_TASKRUN.
    //
    bool deferTaskrun = false;

    // IORING_SETUP_SINGLE_ISSUER is always forced and is not configurable — the cooperative
    // model guarantees only the cooperator's thread submits to the ring.

    // IORING_SETUP_ATTACH_WQ: share the kernel worker queue (including SQPOLL thread) with an
    // existing ring identified by its fd. Set to -1 (default) for an independent queue, or to
    // the fd of another ring to share its workers.
    //
    int attachSqFd = -1;
};

static const UringConfiguration s_defaultUringConfiguration = {
    .entries = 64,
    .registeredSlots = 64,
    .taskName = "Uring",
    .sqpoll = false,
    .sqpollIdleMs = 0,
    .iopoll = false,
    .coopTaskrun = true,
    .deferTaskrun = false,
};

} // end namespace io
} // end namespace coop
