# Buffer-ring multishot recv

## Why

coop's recv is caller-owned: every recv names a userspace buffer at submit time. A connection that
wants to stay armed for the next byte must therefore pin a recv buffer for its whole lifetime, even
while idle. For a server holding many mostly-idle keep-alive connections, that is connection-count ×
bufsize of resident memory doing nothing — the dominant recv-side cost at C10K fan-out is the
buffers, not the connections.

A provided buffer ring (`IORING_REGISTER_PBUF_RING`, kernel 5.19+) inverts the ownership. The
application hands the kernel a pool of buffers once; a recv then names only the pool, by group id,
and the kernel selects a buffer — reporting its id in the completion — only when bytes actually
arrive. An idle connection holds no buffer. Resident recv memory tracks in-flight depth, not
connection count.

The buffer ring is also the hard prerequisite for **multishot recv** (kernel 6.0+): one submitted
recv that stays armed and emits a completion per arrival. Multishot has nowhere to put its data
without a kernel-selected buffer, and a connection served by one armed recv for its whole life pays
no per-message submission cost.

## Mechanism

Three pieces, all opt-in. A Uring with no buffer ring and no armed recv behaves exactly as before.

### `BufferRing` (`coop/io/buffer_ring.h`, header-only)

Registers a pbuf ring, seeds every slot, decodes the kernel-selected buffer id out of a recv
completion (`IORING_CQE_F_BUFFER` / `cqe->flags >> IORING_CQE_BUFFER_SHIFT`), and recycles consumed
buffers back to the kernel. Recycle is batched: `Return(bid)` stages a slot, `Publish()` makes the
staged slots visible to the kernel in one ring advance.

### `ArmedHandle` (`coop/io/armed_handle.{h,cpp}`)

The multishot-aware sibling of `io::Handle`, and a separate type for a load-bearing reason.
`io::Handle` models exactly one logical operation with a fixed completion count: it acquires its
coordinator at submit, decrements a pending count per completion, and releases at zero. That
`count == 0 → Release` invariant is also what the `Handle` destructor's Cancel/Flash drain depends
on, so it must not be bent. A multishot recv breaks the invariant — one submitted recv yields an
unbounded completion stream — so it lives on a distinct path rather than as an edit to `Handle`.

`ArmedHandle` holds its coordinator continuously across the whole stream (mirroring `Handle`'s "held
across the op", extended), surfaces one received chunk per completion to a blocking `Next()`
consumer, recycles each buffer on the following `Next()`, re-arms transparently when the kernel drops
`IORING_CQE_F_MORE` with data still flowing, and surfaces `-ENOBUFS` (pool drained, kernel disarmed)
so the caller can recycle and re-arm. Its destructor cancels an in-flight multishot and blocks on
the coordinator until the cancel acknowledgment and terminal completion drain, exactly as
`io::Handle`'s destructor does.

Per-connection armed state is O(1) in the idle case: the chunk queue is allocated lazily on the
first delivered byte, so an armed-but-idle connection holds only the `ArmedHandle` itself, not a
queue sized to the pool. (When it is allocated, it is sized to a hard bound the pool can never
exceed, because at most pool-many buffers can be checked out at once.)

### `Uring::Init` feature probe

When `UringConfiguration::bufferRingEntries > 0`, Init registers a single default buffer ring,
reachable via `Uring::GetBufferRing()`. The registration doubles as the runtime feature probe: on a
kernel without pbuf-ring support the register call fails, and Init warns and continues with no
default ring — classic recv is untouched — mirroring the registered-ring-fd fallback already in
Init. Code that needs several pools constructs `BufferRing` instances directly.

### Reconciliation with the batched CQ-head advance

`Uring::Poll` reaps a whole ready batch of completions and advances the CQ head once with a single
`io_uring_cq_advance`, rather than per completion. `ArmedHandle`'s completion callbacks therefore do
**not** call `io_uring_cqe_seen` — a per-completion advance would double-advance the kernel head.
The completion pointer stays valid for the duration of the callback, which is all the buffer-id and
result reads require. `Handle::Callback` routes armed-tagged userdata (bit 1) to
`ArmedHandle::Dispatch` before the one-shot decode; within each species bit 0 disambiguates
(linked-timeout/cancel for `Handle`, recv-vs-cancel-ack for `ArmedHandle`).

## Measured (kernel 6.1, this host)

`benchmarks/bench_buffer_ring_throughput.cpp` runs a closed-loop ping-pong echo — drivers keep one
in-flight request per connection, an echo handler recvs each message and sends it straight back — the
same load two ways: classic blocking one-shot recv into a private buffer per connection, vs one armed
multishot recv per connection drawing from a shared pool registered through the Init path. It reports
throughput and the idle-armed resident-memory slope side by side.

- **Throughput**: armed multishot is consistently faster, ~1.1–1.4× classic at 1000 connections /
  64-byte messages, the margin widening with connection count. One armed recv streams completions for
  a connection's whole life, so the steady state spends no submission re-arming a recv per message —
  the cost that scales with load. The throughput win is real, not just a memory trade.
- **Idle recv memory**: classic costs ~2.6–3.5 KiB per armed connection (a pinned recv buffer each);
  armed's marginal cost collapses toward zero (~0–0.26 KiB/conn). The pool is a fixed cost
  independent of connection count.

Honest boundary: a *saturating* closed loop (every connection in flight at once) needs a pool sized
to the offered concurrency, so under full saturation the pool tracks peak concurrency rather than
shrinking. The memory decoupling is a keep-alive / mostly-idle story — which the idle-armed slope
isolates — not a saturation story.

## The push/pull impedance

io_uring multishot is a **push** model: the kernel decides when a completion lands and which buffer
it used. coop consumers are **pull**: a handler calls `Recv` (or `ArmedHandle::Next`) when it is
ready for the next message. The two do not compose for free.

`ArmedHandle` bridges them with a per-connection queue and a single owning-and-consuming context:
pushed completions enqueue `(buffer, len)` and wake the parked consumer; `Next()` pops one and
recycles the previous buffer. This is the **clean blocking-pull consumer** an HTTP-style handler can
drive directly (the echo benchmark's armed handler is exactly that shape: loop on `Next()`, send the
chunk back, repeat). It is single-cooperator and atomic-free, like the one-shot path.

What it does **not** yet do is fan a connection's completions out to a *different* context than the
one that armed it — the multishot-push-to-detached-continuation-per-completion delivery that would
let an arming context and a consuming context differ. The queue, the buffer-recycle handshake, and
the back-pressure story for that cross-context form are unbuilt. The bounded queue already caps
outstanding buffers at pool size, which is the right back-pressure shape, but a detached-continuation
delivery would need to decide what happens when a consumer falls behind (drop, stall the ring, or
surface ENOBUFS upward) before it is safe to land.

## Covenants

Negative-first — what this design forbids:

- **No edits to `io::Handle`'s count-to-zero release invariant.** The one-shot
  `count == 0 → Release` is shared by the sacred destructor Cancel/Flash drain. Multishot lives on
  the distinct `ArmedHandle` path precisely so that invariant is never bent to accommodate an
  unbounded completion stream.
- **No per-completion CQ-head advance on the armed path.** `Uring::Poll` owns the single batched
  `io_uring_cq_advance`. An `ArmedHandle` callback that called `io_uring_cqe_seen` would
  double-advance the kernel head and corrupt the ring.
- **No O(pool) per-connection state for an idle connection.** The per-connection chunk queue is
  allocated lazily on the first delivered byte. Eagerly sizing it to the pool would charge every
  armed-but-idle connection the pool cost and invert the memory win the buffer ring exists to
  deliver.
- **No silent fallback to a degraded mode.** Absent kernel support, Init warns and continues with
  classic recv; it does not pretend a buffer ring exists. A caller that asked for one and did not get
  it sees `GetBufferRing() == nullptr`.
- **No dropped ENOBUFS.** Pool exhaustion is surfaced to the consumer, never swallowed. A consumer
  that ignores it stalls its own connection; it does not silently lose the stream or the wakeup.
- **No suspension inside a pushed completion.** Completion callbacks run to completion and must not
  Yield/Block (the debug `ThunkScope` guard enforces the analogous rule for continuation bodies);
  the consuming context blocks in `Next()`, never the dispatch.
