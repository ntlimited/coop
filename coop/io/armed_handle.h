#pragma once

#include <cstdint>
#include <vector>

struct io_uring_cqe;
struct io_uring_sqe;

namespace coop
{

struct Context;
struct Coordinator;

namespace io
{

struct BufferRing;
struct Descriptor;
struct Uring;

namespace detail
{

// CQE router for armed-tagged userdata (bit 1). Bit 2 selects the armed species
// (recv / accept); bit 0 distinguishes each species' cancel acknowledgment. Called from
// Handle::Callback.
//
void ArmedDispatch(struct io_uring_cqe* cqe, uintptr_t data);

} // end namespace coop::io::detail

// ArmedHandleImpl: the multishot lifecycle core, shared by every armed species.
//
// Why a separate lifecycle from io::Handle
// ----------------------------------------
//
// io::Handle models exactly one logical operation with a fixed CQE count: it acquires its
// coordinator at Submit, decrements a pending count per CQE, and releases at zero. That
// count==0->Release invariant is what the Handle destructor's Cancel/Flash drain depends
// on, so it is load-bearing and must not be bent.
//
// A multishot op breaks that invariant: one submitted SQE produces an unbounded stream of
// CQEs — each carrying IORING_CQE_F_MORE while the operation stays armed — ending only on
// error, resource exhaustion, or benign kernel re-arm (F_MORE is NOT sticky). The armed
// lifecycle holds the coordinator continuously from the first Arm() and releases only at
// teardown once every outstanding CQE has drained. A consumer context parks on it via the
// species' Next(); each surfaced CQE wakes it. Single-cooperator and atomic-free.
//
// The owning context and the consuming context are the same: Next() and the destructor
// both run on m_context.
//
// What lives here vs in the species
// ---------------------------------
//
// Here: the coordinator-held-across-the-stream protocol, the bounded surfaced-item queue,
// consumer park/wake, cancel issuance, and the teardown drain (Cancel + Flash + wake).
// The species (ArmedHandle = multishot recv, ArmedAccept = multishot accept) supply SQE
// prep, CQE decoding, item disposal on teardown, and their re-arm/backpressure policy via
// OnCqe. Every species destructor MUST call TeardownDrain() first — the drain runs
// species callbacks, so it has to complete while the species is still alive.
//
template<typename Derived, typename Entry>
struct ArmedHandleImpl
{
    ArmedHandleImpl(ArmedHandleImpl const&) = delete;
    ArmedHandleImpl& operator=(ArmedHandleImpl const&) = delete;

    ArmedHandleImpl(Context*, Descriptor&, Coordinator*);
    ~ArmedHandleImpl();

    // Submit the multishot SQE (species PrepSqe) and hold the coordinator. No-op
    // acquisition once held (the steady state across re-arms).
    //
    void Arm();

    bool Armed() const { return m_armed; }
    uint64_t Delivered() const { return m_delivered; }

protected:
    // One surfaced completion: the species' item plus the result it reports from Next().
    //
    struct Slot
    {
        Entry   entry;
        int32_t res;
    };

    // Species dtors call this first; asserts in the base dtor enforce it.
    //
    void TeardownDrain();

    // Cancel the live multishot (teardown or species backpressure). The cancel ack and
    // the terminal CQE both route back through the species OnCqe/OnCancelAck.
    //
    void Cancel();

    void EnqueueSlot(Entry entry, int32_t res);
    Slot DequeueSlot();
    void WakeConsumer();
    void MaybeReleaseForTeardown();

    // Core of the species' Next(): recycle-previous is species-side; this parks until a
    // slot or terminal state is available.
    //
    int NextSlot(Entry* out);

    Uring*       m_ring;
    Descriptor*  m_descriptor;
    Coordinator* m_coord;
    Context*     m_context;

    // Bounded circular queue of surfaced slots, allocated lazily on first use (an idle
    // armed connection must hold essentially nothing). Capacity from Derived::QueueBound.
    //
    std::vector<Slot> m_queue;
    uint32_t m_qHead{0};
    uint32_t m_qTail{0};
    uint32_t m_qCount{0};

    int32_t  m_finalResult{0};

    bool     m_armed{false};
    bool     m_cancelPending{false};
    bool     m_consumerParked{false};
    bool     m_tearingDown{false};
    bool     m_drained{false};

    uint64_t m_delivered{0};
};

// ArmedHandle: multishot recv over a provided-buffer ring.
//
// Paired with a BufferRing, a single armed recv serves a connection for its whole
// lifetime without pinning a userspace recv buffer per connection: the kernel selects a
// pool buffer only when bytes actually land. Resident recv memory tracks in-flight
// depth, not connection count.
//
struct ArmedRecvEntry
{
    char*   data = nullptr;
    int32_t bid  = -1;          // -1 = no kernel buffer (terminal slots, default Entry)
};

struct ArmedHandle final : ArmedHandleImpl<ArmedHandle, ArmedRecvEntry>
{
    // A chunk of received bytes surfaced from one CQE. data points into the BufferRing
    // slot and is valid until the next Next() call (which returns the slot to the
    // kernel). bid is the kernel buffer id, or -1 for a terminal completion.
    //
    struct Chunk
    {
        char*   data;
        int32_t len;
        int32_t bid;
    };

    using Entry = ArmedRecvEntry;

    static constexpr uintptr_t kTypeTag = 0x0;

    ArmedHandle(Context*, Descriptor&, BufferRing*, Coordinator*);
    ~ArmedHandle();

    // Block until the next chunk is available, returning its length:
    //   len  > 0 : data chunk (out->data / out->len valid)
    //   len == 0 : peer closed (EOF); the stream is finished
    //   len  < 0 : negative errno (e.g. -ENOBUFS — recycle buffers and Arm() to resume)
    // The chunk's buffer is automatically recycled to the ring on the following Next().
    //
    int Next(Chunk* out);

    uint64_t Enobufs() const { return m_enobufs; }

    // Species hooks for the armed core
    //
    void PrepSqe(struct io_uring_sqe* sqe);
    void OnCqe(struct io_uring_cqe* cqe);
    void OnCancelAck(struct io_uring_cqe* cqe);
    uint32_t QueueBound() const;
    bool ResumeOnEmpty() { return false; }   // recv never self-pauses
    bool ParkKillAware() const { return false; } // post-kill drain reads are legal

    static void Dispatch(struct io_uring_cqe* cqe, uintptr_t data);

private:
    BufferRing* m_bufferRing;
    int32_t     m_returnBid{-1};   // buffer to recycle on the next Next(), or -1
    uint64_t    m_enobufs{0};
};

static_assert(alignof(ArmedHandle) >= 8, "ArmedHandle must be 8-byte aligned for tagged userdata");

} // end namespace io
} // end namespace coop
