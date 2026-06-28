#pragma once

#include <cstring>
#include <liburing.h>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "descriptor.h"
#include "uring_configuration.h"

#include "coop/coordinator.h"
#include "coop/detail/embedded_list.h"

namespace coop
{

struct Context;

namespace io
{

struct BufferRing;

// the coop::io::Uring serves as a wrapper around io_uring, as wrapped by liburing. It is not
// expected that most developers will need to interact with the uring in any direct fashion:
// instead, the
//
struct Uring
{
    using DescriptorList = EmbeddedList<Descriptor>;

    // Constructor and destructor are both out-of-line so std::unique_ptr<BufferRing> can hold an
    // incomplete type in this header; their definitions in uring.cpp see the full BufferRing (the
    // constructor for exception-cleanup of the member, the destructor for its unregister-on-destroy).
    //
    Uring(UringConfiguration const& config = s_defaultUringConfiguration);
    ~Uring();

    int PendingOps() const { return m_pendingOps; }
    int RingFd() const { return m_ring.ring_fd; }

    // The default provided buffer ring registered by Init when UringConfiguration::bufferRingEntries
    // is set and the kernel supports pbuf rings; nullptr otherwise (feature absent or not requested).
    //
    BufferRing* GetBufferRing() const { return m_bufferRing.get(); }

    // True when io_uring has completions waiting to be harvested -- either CQEs already sitting in
    // the completion ring, or, under COOP_TASKRUN, kernel task_work that will materialize CQEs on
    // the next io_uring_enter(). It is a pure userspace read of kernel-mapped ring memory (the CQ
    // head/tail and the SQ kflags word), no syscall. The continuation drain uses it to yield back
    // to the scheduler the instant real IO is ready, rather than running a fixed thunk budget.
    //
    bool HasPendingCompletions() const;

    // True when SQEs have been queued (Handle::Submit et al.) but not yet flushed to the kernel with
    // io_uring_submit. Deferred submission batches these to the batch-boundary Poll for throughput,
    // but a fastpath that defers the loop indefinitely leaves them unsubmitted -- a timer never arms,
    // an async op never starts. The directYield governor reads this to bound submission staleness.
    //
    bool HasPendingSubmissions() const { return m_pendingSqes > 0; }

    void Init();

    // Submit any pending SQEs to the kernel. Returns the number of SQEs submitted (from
    // io_uring_submit), or 0 if nothing was pending.
    //
    int Submit();

    // Process any available CQEs without blocking. Submits pending SQEs first (deferred
    // submission), then processes completions. Returns the number of CQEs dispatched.
    //
    int Poll();

    // Process already-materialized CQEs without entering the kernel — no io_uring_submit and
    // no get_events. The scheduler calls this between resumes within a batch so that SQEs armed
    // by successive resumes accumulate into a single submit at the batch boundary, rather than
    // one io_uring_enter per resume.
    //
    // Under COOP_TASKRUN completions only surface on an io_uring_enter, so mid-batch this only
    // dispatches what a prior submit already delivered; the batch-boundary Poll() is what flushes
    // the accumulated SQEs and harvests their task_work. Returns the number of CQEs dispatched.
    //
    int ReapOnly();

    // Block until at least one CQE is available. Submits pending SQEs first. Used by the
    // cooperator when all contexts are blocked on IO — replaces a tight spin with an efficient
    // kernel wait. Returns number of CQEs dispatched.
    //
    int WaitAndPoll();

    // Get an SQE from the submission ring. If the ring is full, flushes pending SQEs to the
    // kernel and retries. Returns nullptr only if the ring is truly exhausted (shouldn't happen
    // in normal operation).
    //
    struct io_uring_sqe* GetSqe();

    void Run(Context* ctx);

    // TODO lock down the guts
    //

    friend struct Descriptor;
    friend struct Handle;

    // Register/Unregister handle kernel fd registration only. The descriptor list is managed
    // directly by Descriptor constructors/destructors.
    //
    void Register(Descriptor*);
    void Unregister(Descriptor*);

    struct  io_uring m_ring;
    DescriptorList m_descriptors;
    int m_pendingOps{0};
    int m_pendingSqes{0};

    // io_uring fd registration table. Slots contain the real fd or -1 for empty. Registration is
    // opt-in via the Descriptor(Registered, ...) constructor. When a descriptor is registered, its
    // slot index is stored in Descriptor::m_registeredIndex and operations use IOSQE_FIXED_FILE.
    //
    std::vector<int> m_registered;
    UringConfiguration m_config;

    // Optional default provided buffer ring, registered by Init when configured and supported.
    // Held by pointer so its registration outlives Init and is torn down with the Uring.
    //
    std::unique_ptr<BufferRing> m_bufferRing;
};

} // end namespace io

} // end namespace coop
