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

// Handle to an event being processed by io_uring. Because it needs to hook into the user data for
// the SQE, note that it is responsible for completing the contract for the uring submission after
// the initial io_uring_prep_... by the caller.
//
struct Handle : EmbeddedListHookups<Handle>
{
    using List = EmbeddedList<Handle>;

    Handle(Handle const&) = delete;
    Handle(Handle&&) = delete;
    
    // TODO this contract is weird because I wanted to do a cute trick with operator int() without
    // using Self() to get the current context. It's probably bad and I should feel bad.
    //
    Handle(Context*, Descriptor*, Coordinator*, struct io_uring_sqe*);

    Handle(int result)
    : m_context(nullptr)
    , m_coord(nullptr)
    , m_descriptor(nullptr)
    , m_result(result)
    {
        printf("Created io handle at %p\n", this);
    }

    // Either we have completed the operation and are done or we were cancelled.
    //
    ~Handle()
    {
        assert(Disconnected());
    }

    operator int();

    // Cancel is weird because we have to guarantee that the cancel is acked before we actually stop.
    // io_uring probably does do a CQE for this if I had to guess.
    //
    // void Cancel()
    //

    // Invoked when the completion queue event is received.
    //
    void Complete(Context* ctx, struct io_uring_cqe*);

    static void Callback(Context* ctx, struct io_uring_cqe* cqe);

  private:
    Descriptor*     m_descriptor;
    Coordinator*    m_coord;
    Context*        m_context;

    int m_result;
};

} // end namespace coop::io
} // end namespace coop
