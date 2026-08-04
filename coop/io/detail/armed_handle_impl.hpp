#pragma once

// Member definitions for ArmedHandleImpl<Derived>. Included by the .cpp of each armed
// species, which then explicitly instantiates its own ArmedHandleImpl — the template
// stays out of headers so liburing does too.

#include <cassert>
#include <cerrno>
#include <liburing.h>

#include "coop/io/armed_handle.h"
#include "coop/io/descriptor.h"
#include "coop/io/uring.h"

#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"

namespace coop
{

namespace io
{

// Tagged userdata bits for armed CQEs. Bit 1 marks "armed" (routed by Handle::Callback
// to detail::ArmedDispatch); bit 2 selects the species; bit 0 marks that species'
// cancel acknowledgment. The low 3 bits are free because every species static_asserts
// 8-byte alignment.
//
inline constexpr uintptr_t kArmedTag   = 0x2;
inline constexpr uintptr_t kCancelTag  = 0x1;
inline constexpr uintptr_t kSpeciesTag = 0x4;

template<typename Derived, typename Entry>
ArmedHandleImpl<Derived, Entry>::ArmedHandleImpl(
    Context* context,
    Descriptor& descriptor,
    Coordinator* coordinator)
: m_ring(descriptor.m_ring)
, m_descriptor(&descriptor)
, m_coord(coordinator)
, m_context(context)
{
}

template<typename Derived, typename Entry>
ArmedHandleImpl<Derived, Entry>::~ArmedHandleImpl()
{
    // The drain runs species callbacks (CQE decode, item disposal), so it must complete
    // while the species is still alive — every species destructor calls TeardownDrain()
    // before this base destructor runs.
    //
    assert(m_drained && "species destructor must call TeardownDrain() first");
}

template<typename Derived, typename Entry>
void ArmedHandleImpl<Derived, Entry>::TeardownDrain()
{
    m_tearingDown = true;

    if (m_armed || m_cancelPending)
    {
        // A multishot (or its cancel) is still live in the kernel. Cancel and
        // cooperatively block until the acknowledgment and the terminal CQE drain; the
        // drain path releases the coordinator, which wakes this Flash. Mirrors
        // io::Handle's destructor contract.
        //
        Cancel();
        m_coord->Flash(m_context);
    }
    else if (m_coord->IsHeld())
    {
        // Stream already ended but the coordinator is still held from Arm(). Nothing is
        // in flight, so release directly.
        //
        m_coord->Release(m_context, false);
    }

    m_drained = true;
}

template<typename Derived, typename Entry>
void ArmedHandleImpl<Derived, Entry>::Arm()
{
    assert(!m_armed && "Arm() called while a multishot is already live");

    auto* sqe = m_ring->GetSqe();
    assert(sqe);

    static_cast<Derived*>(this)->PrepSqe(sqe);

    if (m_descriptor->m_registeredIndex >= 0)
    {
        sqe->fd = m_descriptor->m_registeredIndex;
        sqe->flags |= IOSQE_FIXED_FILE;
    }

    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(this) | kArmedTag | Derived::kTypeTag));

    // No-op once the coordinator is already held (the steady state across re-arms).
    //
    m_coord->TryAcquire(m_context);
    m_ring->m_pendingOps++;
    m_armed = true;
}

template<typename Derived, typename Entry>
void ArmedHandleImpl<Derived, Entry>::Cancel()
{
    if (!m_armed || m_cancelPending)
    {
        return;
    }

    auto* sqe = m_ring->GetSqe();
    assert(sqe);

    // Target the multishot SQE by its tagged userdata; the cancel's own userdata adds
    // the cancel tag so its completion routes to the species OnCancelAck.
    //
    io_uring_prep_cancel(sqe, reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(this) | kArmedTag | Derived::kTypeTag), 0);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(this) | kArmedTag | Derived::kTypeTag | kCancelTag));

    m_cancelPending = true;
    m_ring->m_pendingOps++;
}

template<typename Derived, typename Entry>
void ArmedHandleImpl<Derived, Entry>::WakeConsumer()
{
    if (m_consumerParked)
    {
        // Consume the park token before releasing so a burst of CQEs in one Poll wakes
        // the consumer exactly once and keeps the coordinator held across the hand-off.
        //
        m_consumerParked = false;
        m_coord->Release(m_context, false);
    }
}

template<typename Derived, typename Entry>
void ArmedHandleImpl<Derived, Entry>::MaybeReleaseForTeardown()
{
    if (m_tearingDown && !m_armed && !m_cancelPending)
    {
        m_coord->Release(m_context, false);
    }
}

template<typename Derived, typename Entry>
void ArmedHandleImpl<Derived, Entry>::EnqueueSlot(Entry entry, int32_t res)
{
    if (m_queue.empty())
    {
        // First slot: size the queue to the species' hard bound (for recv, the buffer
        // pool size — a checked-out buffer is one not yet recycled; for accept, the
        // configured pending bound), plus headroom for a terminal marker.
        //
        m_queue.resize(static_cast<const Derived*>(this)->QueueBound() + 4);
    }
    assert(m_qCount < m_queue.size());
    m_queue[m_qTail] = Slot{entry, res};
    m_qTail = (m_qTail + 1) % m_queue.size();
    m_qCount++;
}

template<typename Derived, typename Entry>
typename ArmedHandleImpl<Derived, Entry>::Slot ArmedHandleImpl<Derived, Entry>::DequeueSlot()
{
    assert(m_qCount > 0);
    Slot s = m_queue[m_qHead];
    m_qHead = (m_qHead + 1) % m_queue.size();
    m_qCount--;
    return s;
}

template<typename Derived, typename Entry>
int ArmedHandleImpl<Derived, Entry>::NextSlot(Entry* out)
{
    while (m_qCount == 0)
    {
        if (!m_armed)
        {
            // Disarmed with nothing buffered. A species that paused itself for
            // backpressure resumes here (an empty queue always satisfies its drain
            // threshold) or reports that it is still waiting on its cancel ack — in
            // either case fall through to park; the ack or the next CQE wakes us. A
            // genuinely finished stream reports the terminal result idempotently.
            //
            if (!static_cast<Derived*>(this)->ResumeOnEmpty())
            {
                *out = Entry{};
                return m_finalResult;
            }
        }

        m_consumerParked = true;
        if (static_cast<Derived*>(this)->ParkKillAware())
        {
            // Kill-aware park: a kill wakes the consumer even when the kernel never
            // terminates the armed op (a listener shutdown does not reliably complete a
            // multishot accept). The coordinator stays held either way — the teardown
            // drain (Cancel + Flash) runs from exactly this state.
            //
            auto r = CoordinateWithKill(m_context, m_coord);
            m_consumerParked = false;
            if (r.Killed())
            {
                *out = Entry{};
                return -ECANCELED;
            }
        }
        else
        {
            CoordinateWith(m_context, m_coord);
            m_consumerParked = false;
        }
    }

    Slot s = DequeueSlot();
    *out = s.entry;
    return s.res;
}

} // end namespace coop::io
} // end namespace coop
