#include <liburing.h>

#include "handle.h"

#include "descriptor.h"
#include "uring.h"

#include "coop/context.h"
#include "coop/coordinator.h"

namespace coop
{

namespace io
{

Handle::Handle(
    Context* context,
    Descriptor* descriptor,
    Coordinator* coordinator,
    struct io_uring_sqe* sqe)
: m_descriptor(descriptor)
, m_coord(coordinator)
, m_context(context)
{
    #if 0
    if (descriptor->m_registered)
    {
        sqe->flags |= IOSQE_FIXED_FILE;
        auto old = sqe->fd;
        sqe->fd = desc.m_ring->RegisteredIndex(desc.m_registered);
        printf("Replaced fd %d with registered fd %d\n", old, sqe->fd);
    }
    #endif

    m_coord->TryAcquire(m_context);
    m_descriptor->m_handles.Push(this);

    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(this));
    io_uring_submit(&descriptor->m_ring->m_ring);
}

Handle::operator int()
{
    if (m_context)
    {
        m_coord->Flash(m_context);
    }

    return m_result;
}

void Handle::Complete(Context* ctx, struct io_uring_cqe* cqe)
{
    assert(m_context);

    m_result = cqe->res;
    io_uring_cqe_seen(&m_descriptor->m_ring->m_ring, cqe);

    m_context = nullptr;
    // TODO is this useful even?
    //
    this->Pop();
    
    m_coord->Release(ctx, false /* schedule */);
}

void Handle::Callback(Context* ctx, struct io_uring_cqe* cqe)
{
    reinterpret_cast<Handle*>(io_uring_cqe_get_data(cqe))->Complete(ctx, cqe);
}

} // end namespace coop::io
} // end namespace coop
