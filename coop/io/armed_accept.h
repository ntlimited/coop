#pragma once

#include <cstdint>

#include "armed_handle.h"

namespace coop
{

namespace io
{

// ArmedAccept: multishot accept over a listening descriptor — one SQE serves the
// listener's whole lifetime, each connection surfacing as one CQE, replacing the
// per-accept Coordinator/Handle/SQE ceremony of the one-shot loop.
//
// Backpressure covenant
// ---------------------
//
// Unlike armed recv, accept has no provided-buffer pool to exhaust: the kernel will keep
// completing accepts as fast as SYNs arrive, and every queued completion is a REAL
// kernel fd charged to the process. An acceptor that falls behind must not convert a
// connection storm into fd exhaustion. The backpressure here is the listen backlog:
// when the surfaced-but-unconsumed queue reaches `maxPending`, the multishot is
// cancelled — the kernel stops accepting, the SYN/accept backlog fills, and remote
// peers queue or shed at the TCP layer. Draining below half the bound re-arms.
//
// Accepted fds are created with SOCK_NONBLOCK — required by the splice/sendfile data
// engines and the recv/send fastpaths, and cheaper than the fcntl pair per accept.
//
struct ArmedAcceptEntry
{
    int fd = -1;
};

struct ArmedAccept final : ArmedHandleImpl<ArmedAccept, ArmedAcceptEntry>
{
    using Entry = ArmedAcceptEntry;

    static constexpr uintptr_t kTypeTag = 0x4;

    // maxPending is the pause threshold (soft): reaching it cancels the multishot. The
    // cancel is submitted on the next Poll, and one Poll batch can deliver a backlog's
    // worth of CQEs past the threshold — the queue absorbs that burst up to
    // max(2*maxPending, maxPending+16), beyond which surfaced connections are shed
    // (closed, peer sees a reset) and counted. Each queued entry is a real kernel fd.
    //
    ArmedAccept(Context*, Descriptor& listener, Coordinator*, uint32_t maxPending = 64);
    ~ArmedAccept();

    // Block until the next accepted connection:
    //   ret >= 0 : a connected socket fd (SOCK_NONBLOCK)
    //   ret <  0 : negative errno; the stream is finished (listener closed/broken)
    // Consuming an entry may re-arm a paused multishot (backpressure drain).
    //
    int Next();

    uint64_t Paused() const { return m_pauses; }
    uint64_t Shed() const { return m_sheds; }

    // Species hooks for the armed core
    //
    void PrepSqe(struct io_uring_sqe* sqe);
    void OnCqe(struct io_uring_cqe* cqe);
    void OnCancelAck(struct io_uring_cqe* cqe);
    uint32_t QueueBound() const
    {
        return m_maxPending + 16 > 2 * m_maxPending ? m_maxPending + 16
                                                    : 2 * m_maxPending;
    }

    // An empty queue always satisfies the drain threshold: resume a backpressure pause
    // rather than reporting a phantom terminal. True = live again (or still waiting on
    // the cancel ack, which will resume) — park rather than report terminal.
    //
    bool ResumeOnEmpty()
    {
        TryResume();
        return m_armed || m_pausing || m_cancelPending;
    }

    static void Dispatch(struct io_uring_cqe* cqe, uintptr_t data);

private:
    void TryResume();

    uint32_t m_maxPending;
    bool     m_pausing{false};   // Cancel issued for backpressure, not teardown
    uint64_t m_pauses{0};
    uint64_t m_sheds{0};
};

static_assert(alignof(ArmedAccept) >= 8, "ArmedAccept must be 8-byte aligned for tagged userdata");

} // end namespace io
} // end namespace coop
