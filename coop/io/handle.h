#pragma once

#include "coop/embedded_list.h"

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

// Handle to an event being processed by io_uring. Because it needs to hook into the user data for
// the SQE, note that it is responsible for completing the contract for the uring submission after
// the initial io_uring_prep_... by the caller.
//
struct Handle : EmbeddedListHookups<Handle>
{
    using List = EmbeddedList<Handle>;

    Handle(Handle const&) = delete;
    Handle(Handle&&) = delete;

    Handle(Context*, Descriptor&, Coordinator*);
    Handle(Context*, Uring*, Coordinator*);

    // Either we have completed the operation and are done or we were cancelled.
    //
    ~Handle()
    {
        assert(Disconnected());
    }

    operator int();

    void Submit(struct io_uring_sqe*);

    // Cancel is weird because we have to guarantee that the cancel is acked before we actually stop.
    // io_uring probably does do a CQE for this if I had to guess.
    //
    // void Cancel()
    //

    // Invoked when the completion queue event is received.
    //
    void Complete(struct io_uring_cqe*);

    static void Callback(struct io_uring_cqe* cqe);

    // TODO visibility
    //
    Uring*          m_ring;
    Descriptor*     m_descriptor;
    Coordinator*    m_coord;
    Context*        m_context;

    int m_result;
};

} // end namespace coop::io
} // end namespace coop
