#pragma once

#include <cstdint>
#include <vector>

struct io_uring_cqe;

namespace coop
{

struct Context;
struct Coordinator;

namespace io
{

struct BufferRing;
struct Descriptor;
struct Uring;

// ArmedHandle: the multishot-aware sibling of io::Handle.
//
// Why a separate type
// -------------------
//
// io::Handle models exactly one logical operation with a fixed CQE count: it acquires its
// coordinator at Submit, decrements a pending count per CQE, and releases at zero. That
// count==0->Release invariant is also what the Handle destructor's Cancel/Flash drain depends
// on, so it is load-bearing and must not be bent.
//
// A multishot recv breaks that invariant. One submitted SQE produces an unbounded stream of
// CQEs -- each carrying IORING_CQE_F_MORE while the operation stays armed -- and ends only on
// error, on -ENOBUFS (buffer pool drained), or when the kernel re-arms (F_MORE is NOT sticky;
// the kernel periodically drops it and expects a fresh SQE). The one-shot lifecycle cannot
// express "hold the coordinator across a stream and surface a result per CQE", so this is a
// distinct armed path rather than an edit to Handle.
//
// What it buys
// ------------
//
// Paired with a BufferRing, a single armed recv serves a connection for its whole lifetime
// without pinning a userspace recv buffer per connection: the kernel selects a buffer from a
// shared pool only when bytes actually land. Resident recv memory tracks in-flight depth, not
// connection count -- the C10K memory decoupling that one-shot recv cannot offer.
//
// Lifecycle
// ---------
//
//   Arm()                              CQE stream (Uring::Poll)
//     |                                     |
//     v                                     v
//   [ ARMED ]   F_MORE (more coming)   [ OnRecv() ]  enqueue (bid,len) for the consumer
//   coord held  <----------------------       |  res==0  EOF (enqueue, stay disarmed)
//     |         !F_MORE, res>0 re-Arm()        |  res==-ENOBUFS  surface; caller re-arms
//     | ~ArmedHandle / Cancel()                v
//     v                              consumer Next() pops a chunk, recycles its buffer
//   drain cancel + terminal CQE, Release coordinator
//
// The coordinator is held continuously from the first Arm() (mirroring Handle's "held across
// the op", extended across the whole stream) and released only at teardown once every
// outstanding CQE has drained. A consumer context parks on it via Next(); each surfaced CQE
// wakes it. Single-cooperator and atomic-free, like the one-shot path.
//
// The owning context and the consuming context are the same: Next() and the destructor both run
// on m_context. Cross-context fan-out (a detached continuation per CQE) is a later layer.
//
struct ArmedHandle
{
    ArmedHandle(ArmedHandle const&) = delete;
    ArmedHandle& operator=(ArmedHandle const&) = delete;

    // A chunk of received bytes surfaced from one CQE. data points into the BufferRing slot and
    // is valid until the next Next() call (which returns the slot to the kernel). bid is the
    // kernel buffer id, or -1 for a terminal completion (EOF / error) that carries no buffer.
    //
    struct Chunk
    {
        char*   data;
        int32_t len;
        int32_t bid;
    };

    ArmedHandle(Context*, Descriptor&, BufferRing*, Coordinator*);
    ~ArmedHandle();

    // Submit the multishot recv. Holds the coordinator on the first call. Re-arm is automatic on
    // benign multishot termination; callers only call Arm() again after handling -ENOBUFS.
    //
    void Arm();

    // Block until the next chunk is available, returning its length:
    //   len  > 0 : data chunk (out->data / out->len valid)
    //   len == 0 : peer closed (EOF); the stream is finished
    //   len  < 0 : negative errno (e.g. -ENOBUFS -- recycle buffers and Arm() to resume)
    // The chunk's buffer is automatically recycled to the ring on the following Next() call.
    //
    int Next(Chunk* out);

    bool Armed() const { return m_armed; }

    // Diagnostics for tests / observability.
    //
    uint64_t Delivered() const { return m_delivered; }
    uint64_t Enobufs() const { return m_enobufs; }

    // CQE dispatch entry point, routed from Handle::Callback by the armed tag bit. data is the
    // raw (tagged) userdata; bit 0 distinguishes the cancel acknowledgment from a recv CQE.
    //
    static void Dispatch(struct io_uring_cqe* cqe, uintptr_t data);

private:
    void OnRecv(struct io_uring_cqe* cqe);
    void OnCancelAck(struct io_uring_cqe* cqe);

    void Cancel();
    void Enqueue(char* data, int32_t len, int32_t bid);
    Chunk Dequeue();
    void WakeConsumer();
    void MaybeReleaseForTeardown();

    Uring*       m_ring;
    Descriptor*  m_descriptor;
    BufferRing*  m_bufferRing;
    Coordinator* m_coord;
    Context*     m_context;

    // Bounded circular queue of surfaced chunks. Capacity exceeds the buffer pool size, so it
    // can never overflow: a checked-out buffer is one not yet recycled, and there are only pool
    // many buffers.
    //
    std::vector<Chunk> m_queue;
    uint32_t m_qHead{0};
    uint32_t m_qTail{0};
    uint32_t m_qCount{0};

    int32_t  m_returnBid{-1};   // buffer to recycle on the next Next(), or -1
    int32_t  m_finalResult{0};  // result returned once the stream is drained and disarmed

    bool     m_armed{false};
    bool     m_cancelPending{false};
    bool     m_consumerParked{false};
    bool     m_tearingDown{false};

    uint64_t m_delivered{0};
    uint64_t m_enobufs{0};
};

static_assert(alignof(ArmedHandle) >= 8, "ArmedHandle must be 8-byte aligned for tagged userdata");

} // end namespace io
} // end namespace coop
