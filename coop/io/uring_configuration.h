#pragma once

namespace coop
{
namespace io
{

struct UringConfiguration
{
    int entries;
    int registeredSlots;
    const char* taskName;

    // IORING_SETUP_SQPOLL: kernel thread polls the SQ for new entries, avoiding io_uring_enter()
    // for submission. May require CAP_SYS_ADMIN or appropriate cgroup permissions.
    //
    bool sqpoll;

    // IORING_SETUP_IOPOLL: kernel busy-polls for I/O completions instead of using interrupts.
    // Only works with O_DIRECT / pollable operations. NOTE: the current Uring::Run() loop uses
    // io_uring_peek_cqe which does not trigger IOPOLL reaping — enabling this requires Run()
    // adaptations that are not yet implemented.
    //
    bool iopoll;

    // IORING_SETUP_COOP_TASKRUN + IORING_SETUP_TASKRUN_FLAG: defers kernel task_work to the
    // next io_uring_enter() instead of async delivery. Natural fit for the cooperative model.
    // Requires kernel 5.19+ and liburing that checks IORING_SQ_TASKRUN in peek paths (≥2.3).
    //
    bool coopTaskrun;

    // IORING_SETUP_SINGLE_ISSUER is always forced and is not configurable — the cooperative
    // model guarantees only the cooperator's thread submits to the ring.
};

static const UringConfiguration s_defaultUringConfiguration = {
    .entries = 64,
    .registeredSlots = 64,
    .taskName = "Uring",
    .sqpoll = false,
    .iopoll = false,
    .coopTaskrun = false,
};

} // end namespace io
} // end namespace coop
