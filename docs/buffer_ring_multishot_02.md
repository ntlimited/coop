# Buffer-ring multishot recv: cross-context delivery (#16)

Continues `buffer_ring_multishot_01.md`. The foundation (BufferRing, ArmedHandle, the Init feature
probe) landed with a **blocking-pull consumer**: the context that arms the multishot recv is the same
one that drains it via `Next()`. That serves a handler that owns its connection end to end, but it
parks one context per armed connection and cannot serve the high-fan-out shape — one acceptor arms
many connections and hands each off. This is the design for the deferred piece: delivering a
connection's pushed completions to a *different* context than the one that armed it, with bounded
back-pressure.

## The shape

```
arming context  -- arms one multishot recv on the BufferRing, then moves on
kernel          -- pushes a completion per arrival, each naming a selected buffer id
delivery        -- a detached continuation per completion enqueues (bid, len) on the
                   connection's queue and, if a consumer is parked, releases it
consuming ctx   -- pulls (bid, len), processes the bytes, recycles the prior buffer
```

Delivery is a **detached continuation** (in-cooperator, fires straight from the recv CQE, parks no
context) — so an armed-but-idle connection costs only its `ArmedHandle`, no parked context. The
consumer is an ordinary `Context` that blocks when the queue is empty and runs when released.

## Why this stays single-cooperator (the buffer-ownership rule)

A provided buffer belongs to the cooperator whose ring selected it: recycling writes that ring's
buffer-ring tail, which only the owning thread may touch (`SINGLE_ISSUER`). So the whole lifecycle —
arm, deliver, consume, recycle — must stay on the owning cooperator. The delivery continuation is
in-cooperator by construction (continuations never migrate), and the consuming context is *required*
to be co-resident. A consumer that wants to run cross-core copies the bytes out of the provided
buffer and recycles it locally first — the same rule as the continuation/Erg split. (The principled
cross-cooperator hand-back, where a stolen unit holds the buffer and returns it via a response
channel, is the separate #18.)

## Back-pressure is the pool

There is no separate flow-control mechanism: the buffer pool *is* the back-pressure. A buffer is held
from the moment the kernel selects it until the consumer recycles it (on the pull after the one that
delivered it). A slow consumer therefore holds buffers; if it falls far enough behind, the pool
drains, the kernel disarms the multishot with `-ENOBUFS`, and `ArmedHandle` surfaces that; re-arming
happens once the consumer catches up and recycles.

- Fast consumer: the queue stays shallow, buffers cycle, the recv stays armed.
- Slow consumer: buffers accumulate up to pool size, then the connection self-throttles via ENOBUFS —
  it stalls its *own* stream, never the shared ring and never another connection.

The per-connection queue is bounded by pool size (at most pool-many buffers can be checked out) and
is allocated lazily on the first delivered byte (O(1) idle), exactly as in the foundation.

## Covenants

Negative-first — what this design forbids:

- **No cross-cooperator buffer lifecycle.** Arm, deliver, consume, recycle all happen on the owning
  cooperator. The delivery continuation never migrates; the consuming context must be co-resident.
  Cross-core consumers copy out and recycle locally — the cross-cooperator hand-back is #18.
- **No back-pressure mechanism beyond the pool.** A slow consumer self-throttles via pool exhaustion →
  ENOBUFS → re-arm. It must not drop chunks, stall the shared ring, or starve other connections.
- **No O(pool) idle state, no parked context for an idle connection.** The queue is lazily allocated;
  delivery is a detached continuation, not a parked context. An armed-idle connection costs its
  `ArmedHandle` and nothing else.
- **No suspension in the delivery continuation.** It enqueues and releases; it never Yields/Blocks
  (the debug `ThunkScope` guard). The consuming context is the only thing that ever parks.
- **No dropped ENOBUFS / lost wakeup.** Pool exhaustion and the terminal completion are surfaced to
  the consumer; a buffer is recycled exactly once, after the consumer is done with it.

## What this unblocks

A high-fan-out server: one acceptor arms a multishot recv per connection on a shared pool;
per-connection detached-continuation delivery feeds consumer contexts (per connection, or a smaller
handler pool draining several), with resident recv memory tracking in-flight depth, not connection
count. The HTTP-parser integration — feeding multishot's pushed chunks into the pull-based parser —
sits on top of this delivery and is the natural next slice once it lands.
