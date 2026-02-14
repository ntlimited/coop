#include <liburing.h>
#include <spdlog/spdlog.h>

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
    Descriptor& descriptor,
    Coordinator* coordinator)
: m_ring(descriptor.m_ring)
, m_descriptor(&descriptor)
, m_coord(coordinator)
, m_context(context)
{
}

Handle::Handle(
    Context* context,
    Uring* ring,
    Coordinator* coordinator)
: m_ring(ring)
, m_descriptor(nullptr)
, m_coord(coordinator)
, m_context(context)
{
}

void Handle::Submit(struct io_uring_sqe* sqe)
{
    spdlog::trace("handle submit ctx={}", m_context->GetName());

    m_coord->TryAcquire(m_context);
    if (m_descriptor)
    {
        m_descriptor->m_handles.Push(this);
    }

    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(this));
    io_uring_submit(&m_ring->m_ring);
}

Handle::operator int()
{
    m_coord->Flash(m_context);
    return m_result;
}

void Handle::Complete(struct io_uring_cqe* cqe)
{
    m_result = cqe->res;
    spdlog::trace("handle complete result={}", m_result);
    io_uring_cqe_seen(&m_ring->m_ring, cqe);

    if (m_descriptor)
    {
        this->Pop();
    }
    m_coord->Release(m_context, false /* schedule */);
}

void Handle::Callback(struct io_uring_cqe* cqe)
{
    reinterpret_cast<Handle*>(io_uring_cqe_get_data(cqe))->Complete(cqe);
}

} // end namespace coop::io
} // end namespace coop
