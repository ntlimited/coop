#pragma once

#include "coop/detail/embedded_list.h"

struct io_uring_cqe;
struct io_uring_sqe;
struct __kernel_timespec;

namespace coop
{

struct Context;
struct Coordinator;

namespace io
{

struct Descriptor;
struct Uring;

// Handle to an event being processed by io_uring. Because it needs to hook into the user data for
// the SQE, note that it is responsible for completing the contract for the uring submission after
// the initial io_uring_prep_... by the caller.
//
// Tagged pointer dispatch: Handle inherits EmbeddedListHookups<Handle> which contains two pointer
// members (prev, next). This guarantees at least 8-byte alignment on all platforms, so the low 3
// bits of any Handle* are always zero. We steal bit 0 to distinguish primary CQEs (the operation
// itself) from secondary/tagged CQEs (cancel acknowledgments, linked timeout completions). This
// lets io_uring operations that produce two CQEs (cancel, linked timeout) route each CQE to the
// correct handler without additional bookkeeping structures.
//
struct Handle : EmbeddedListHookups<Handle>
{
    using List = EmbeddedList<Handle>;

    Handle(Handle const&) = delete;
    Handle(Handle&&) = delete;

    Handle(Context*, Descriptor&, Coordinator*);
    Handle(Context*, Uring*, Coordinator*);
    ~Handle();

    operator int();

    void Submit(struct io_uring_sqe*);

    // Submit with a linked timeout. The operation SQE gets IOSQE_IO_LINK, a second SQE is
    // appended for the linked timeout, and m_pendingCqes is set to 2. The __kernel_timespec*
    // must remain valid until both CQEs are consumed (guaranteed because the owning context
    // blocks via Flash and its stack frame stays alive).
    //
    void SubmitLinked(struct io_uring_sqe*, struct __kernel_timespec*);

    // Cancel an in-flight operation. Submits an IORING_OP_ASYNC_CANCEL targeting our original
    // SQE's userdata. The cancel acknowledgment CQE is routed to OnSecondaryComplete. No-op if
    // m_pendingCqes == 0.
    //
    void Cancel();

    bool TimedOut() const { return m_timedOut; }

    // Invoked when the primary completion queue event is received (untagged pointer).
    //
    void Complete(struct io_uring_cqe*);

    // Invoked for secondary CQEs: cancel acknowledgments and linked timeout completions. Routed
    // here via the tagged pointer (bit 0 set in userdata).
    //
    void OnSecondaryComplete(struct io_uring_cqe*);

    // Dispatches a CQE to the correct Handle method based on the tag bit in the userdata pointer.
    //
    static void Callback(struct io_uring_cqe* cqe);

    // TODO visibility
    //
    Uring*          m_ring;
    Descriptor*     m_descriptor;
    Coordinator*    m_coord;
    Context*        m_context;

    int     m_result;
    int     m_pendingCqes;
    bool    m_timedOut;

private:
    // Shared finalization logic. Decrements m_pendingCqes and only releases the coordinator
    // when it hits zero.
    //
    void Finalize();
};

} // end namespace coop::io
} // end namespace coop
