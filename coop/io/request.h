#pragma once

#include "coop/detail/embedded_list.h"

#include "coop/coordinator.h"

namespace coop
{

namespace io
{

// A Request is made against a Handle and follows the similar, stack-oriented pattern
//
struct Request : EmbeddedListHookups<Request>
{
    enum class Operation
    {
        READ,
        WRITE,
    };

    using List = EmbeddedList<Request>;

    // Technically we could support move semantics here but currently, no real value.
    //
    Request(Request const&) = delete;
    Request(Request&&) = delete;

    // A request is made to a Handle to take given buffer of the specified size and perform the
    // requested operation.
    //
    Request(Handle* h, void* buf, size_t sz, Operation op, Coordinator* coordinator)
    : m_handle(h)
    , m_buffer(buf)
    , m_size(sz)
    , m_operation(op)
    , m_coordinator(coordinator)
    {
        (op == Operation::READ ? h->m_reads : h->m_writes).Push(this);
    }

  private:
    Handle*         m_handle;
    void*           m_buffer;
    size_t          m_size;
    Operation       m_operation;
    Coordinator*    m_coordinator;
};

} // end namespace coop::io
} // end namespace coop
