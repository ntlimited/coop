#pragma once

#include <linux/time_types.h>

#include "coop/detail/embedded_list.h"
#include "coop/time/interval.h"

struct io_uring_cqe;
struct io_uring_sqe;

namespace coop
{

struct Context;
struct Coordinator;

namespace io
{

struct Descriptor;
struct Uring;

namespace detail { struct HandleExtension; }

// Handle to an event being processed by io_uring. Each Handle owns exactly one logical operation
// and tracks all CQEs that operation will produce. The address of the Handle is passed to io_uring
// as SQE userdata; CQE callbacks route back through it. This means the Handle MUST remain alive
// and at a stable address until every CQE has been accounted for.
//
// Lifecycle
// ---------
//
//   Submit(sqe)                               CQE arrives (uring Poll)
//       │                                          │
//       ▼                                          ▼
//   ┌─────────┐   Complete/OnSecondary    ┌──────────────┐
//   │SUBMITTED├──────────────────────────►│  Finalize()  │
//   │ cqes=1  │   (each CQE decrements)  │  --cqes > 0? │
//   └────┬────┘                           └──────┬───────┘
//        │                                       │ cqes == 0
//        │ Cancel()                              ▼
//        │ (submits cancel SQE,           ┌────────────┐
//        │  ++cqes)                       │  COMPLETE   │
//        ▼                                │  Pop, Rel.  │
//   ┌──────────┐   cancel + orig CQEs     │  --pending  │
//   │CANCELLING├─────────────────────────►└────────────┘
//   │ cqes=2+  │   (via Finalize)
//   └──────────┘
//
// The coordinator is TryAcquire'd at Submit and Release'd at Finalize(cqes==0). External code
// blocks on the coordinator (via Flash or CoordinateWith) to wait for completion.
//
// Blocking IO (Wait)
//
//   The blocking IO macros call `return handle.Wait()` which uses CoordinateWith to multiplex
//   the Handle's coordinator with the context's kill signal. If the context is killed, Wait()
//   returns -ECANCELED immediately. The Handle destructor then runs Cancel + Flash to drain
//   any remaining CQEs before the stack unwinds.
//
// Destructor safety
//
//   If pendingCqes > 0 when the Handle is destroyed (typical during shutdown — the context was
//   killed before the IO completed), the destructor submits a cancel and blocks via Flash:
//
//       ~Handle:  Cancel() ──► Flash() ──► [blocked] ──► CQEs arrive ──► [unblocked] ──► done
//
//   Flash blocks the context cooperatively, which preserves the stack frame and therefore the
//   Handle's address. The cooperator loop keeps polling uring (guarded by PendingOps > 0) so
//   the cancel CQE arrives promptly — typically on the very next Poll() cycle. Only after all
//   CQEs have been processed and pendingCqes reaches zero does the destructor return and the
//   stack frame unwind.
//
// Tagged pointer dispatch
//
//   Handle inherits EmbeddedListHookups<Handle> which contains two pointer members (prev, next).
//   This guarantees at least 8-byte alignment, so the low 3 bits of any Handle* are always zero.
//   Bit 0 is stolen to distinguish primary CQEs (the operation itself) from secondary CQEs
//   (cancel acknowledgments, linked timeout completions).
//
struct Handle : EmbeddedListHookups<Handle>
{
    using List = EmbeddedList<Handle>;

    Handle(Handle const&) = delete;
    Handle(Handle&&) = delete;

    Handle(Context*, Descriptor&, Coordinator*);
    Handle(Context*, Uring*, Coordinator*);
    ~Handle();

    // Block until the operation completes or the context is killed. Uses CoordinateWith to
    // multiplex the Handle's coordinator with the context's kill signal.
    //
    int Wait();

    // Return the cached result. Only valid after all CQEs have been accounted for (asserts
    // m_pendingCqes == 0).
    //
    int Result() const;

    bool TimedOut() const { return m_timedOut; }

    void Submit(struct io_uring_sqe*);

    // Submit with a linked timeout. Converts the interval to a __kernel_timespec stored in
    // m_timeout, marks the operation SQE with IOSQE_IO_LINK, appends a linked timeout SQE,
    // and sets m_pendingCqes to 2.
    //
    void SubmitWithTimeout(struct io_uring_sqe*, time::Interval timeout);

    // Cancel an in-flight operation. Submits an IORING_OP_ASYNC_CANCEL targeting our original
    // SQE's userdata. The cancel acknowledgment CQE is routed to OnSecondaryComplete. No-op if
    // m_pendingCqes == 0.
    //
    void Cancel();

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

private:
    friend struct detail::HandleExtension;

    // Submit with a linked timeout using a pre-populated m_timeout. Called by SubmitWithTimeout
    // after converting the interval.
    //
    void SubmitLinked(struct io_uring_sqe*);

    // Shared finalization logic. Decrements m_pendingCqes and only releases the coordinator
    // when it hits zero.
    //
    void Finalize();

    Uring*          m_ring;
    Descriptor*     m_descriptor;
    Coordinator*    m_coord;
    Context*        m_context;

    int     m_result;
    int     m_pendingCqes;
    bool    m_timedOut;

    struct __kernel_timespec m_timeout;
};

} // end namespace coop::io
} // end namespace coop
