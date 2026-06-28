#pragma once

#include <cstdint>
#include <liburing.h>
#include <vector>

#include "uring.h"

namespace coop
{

namespace io
{

// A provided buffer ring (IORING_REGISTER_PBUF_RING, kernel 5.19+): a pool of recv buffers the
// kernel draws from on its own schedule instead of the caller pinning one buffer per submitted
// recv.
//
// Why this exists
// ---------------
//
// coop's recv model is caller-owned: every recv names a userspace buffer at submit time, so a
// keep-alive connection that wants to stay armed must reserve a recv buffer for its whole
// lifetime even while idle. At C10K fan-out that is connection-count * bufsize of resident memory
// doing nothing. A buffer ring inverts the ownership: the application hands the kernel a pool
// once, arms recvs that name only the pool (via a group id), and the kernel selects a buffer id
// -- returned in cqe->flags -- only when bytes actually land. Idle connections hold no buffer.
// Resident recv memory tracks in-flight depth, not connection count.
//
// This is also the hard prerequisite for multishot recv and recv bundles, which have nowhere to
// put their data without a kernel-selected buffer.
//
// Opt-in: a Uring without a BufferRing behaves exactly as before. BufferRing is registered
// explicitly by code that wants late-bound recv buffers.
//
// Lifecycle of a buffer
// ---------------------
//
//   Populate()  -- seed every slot, advance the ring tail so the kernel can see them
//        |
//        v  kernel picks slot `bid` when data arrives
//   recv CQE: IORING_CQE_F_BUFFER set, bid = flags >> IORING_CQE_BUFFER_SHIFT
//        |
//        v  application consumes Buffer(bid) bytes
//   Return(bid) -- hand the slot back; advance so the kernel may reuse it
//
// If the application falls behind and the kernel finds the ring empty, the recv completes with
// -ENOBUFS and (for multishot) the recv is disarmed. Callers MUST handle -ENOBUFS by re-arming,
// optionally after a one-shot classic recv into a private buffer to avoid dropping the wakeup.
//
struct BufferRing
{
    BufferRing(BufferRing const&) = delete;
    BufferRing& operator=(BufferRing const&) = delete;

    // entries must be a power of two. bufSize is the fixed size of every buffer in the pool.
    // group is the buffer-group id named by recvs that draw from this ring; it must be unique
    // among buffer rings on the same Uring.
    //
    BufferRing(uint16_t group, uint32_t entries, uint32_t bufSize)
    : m_group(group)
    , m_entries(entries)
    , m_bufSize(bufSize)
    , m_mask(io_uring_buf_ring_mask(entries))
    , m_storage(size_t(entries) * bufSize)
    {
    }

    // Register the ring with the kernel and seed every slot. Returns 0 on success or a negative
    // errno. Call once, after the owning Uring has been Init()'d.
    //
    int Register(Uring& uring)
    {
        int err = 0;
        m_ring = io_uring_setup_buf_ring(&uring.m_ring, m_entries, m_group, 0, &err);
        if (!m_ring)
        {
            return err;
        }
        m_uring = &uring;
        for (uint32_t b = 0; b < m_entries; b++)
        {
            io_uring_buf_ring_add(m_ring, Slot(b), m_bufSize, b, m_mask, b);
        }
        io_uring_buf_ring_advance(m_ring, m_entries);
        return 0;
    }

    ~BufferRing()
    {
        if (m_ring && m_uring)
        {
            io_uring_unregister_buf_ring(&m_uring->m_ring, m_group);
        }
    }

    uint16_t Group() const { return m_group; }
    uint32_t Entries() const { return m_entries; }

    // The bytes the kernel delivered into slot `bid`. Valid until Return(bid).
    //
    char* Buffer(uint32_t bid) { return Slot(bid); }

    // Hand slot `bid` back to the kernel. Batches: the slot is added to the ring but only becomes
    // visible to the kernel on Publish(). Single-buffer callers can use ReturnAndPublish().
    //
    void Return(uint32_t bid)
    {
        io_uring_buf_ring_add(m_ring, Slot(bid), m_bufSize, bid, m_mask, m_pending++);
    }

    void Publish()
    {
        if (m_pending)
        {
            io_uring_buf_ring_advance(m_ring, m_pending);
            m_pending = 0;
        }
    }

    void ReturnAndPublish(uint32_t bid)
    {
        Return(bid);
        Publish();
    }

    // Decode a recv CQE that selected from this ring. Returns false if the CQE carried no buffer
    // (e.g. an -ENOBUFS or error completion), in which case the caller must re-arm.
    //
    static bool SelectedBuffer(struct io_uring_cqe* cqe, uint32_t* bidOut)
    {
        if (!(cqe->flags & IORING_CQE_F_BUFFER))
        {
            return false;
        }
        *bidOut = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        return true;
    }

private:
    char* Slot(uint32_t b) { return m_storage.data() + size_t(b) * m_bufSize; }

    Uring* m_uring{nullptr};
    struct io_uring_buf_ring* m_ring{nullptr};
    uint16_t m_group;
    uint32_t m_entries;
    uint32_t m_bufSize;
    int m_mask;
    uint32_t m_pending{0};
    std::vector<char> m_storage;
};

} // end namespace io
} // end namespace coop
