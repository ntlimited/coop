# coop/chan — Design Notes

## Primitives

| Type | Role |
|------|------|
| `Channel<T>` / `FixedChannel<T,N>` | Intra-cooperator buffered channel. Two coordinators encode state: `m_recv` held ↔ empty, `m_send` held ↔ full. |
| `Pipe<T,U>` | Transform stage: reads from a `RecvChannel<T>`, writes to an internal `FixedChannel<U,N>`. Shuts down when source closes. |
| `Filter<T>` | Like Pipe but same type; drops items that fail the predicate. |
| `Merge<T>` | Fan-in two `RecvChannel<T>`s into one output channel via `Select`. |
| `Ticker` | Emits ticks at a fixed interval using `CoordinateWithKill` + timeout. |
| `Select` | `CoordinateWith` across multiple channel `m_recv` coordinators; completes the winning recv. |
| `Passage<T,N>` | MPSC bridge from external threads to a single receiver cooperator. See below. |
| `SpscPassage<T,N>` | SPSC bridge with the same API; optimized for exactly one producer thread. |

---

## Passage

### Architecture

```
External thread(s)
  Send() → MpscRing<T,N>  (wait-free CAS on m_tail)
              ↓
         m_wakePending CAS → Submit(wake lambda)
                                    ↓
                             [eventfd write → io_uring CQE → SubmissionDrainer → DrainSubmissions]
                                    ↓
                             wake lambda runs on cooperator:
                               m_recv.Release()   ← O(1), no drain loop
                                    ↓
Consumer cooperator
  Recv() ← ring.Pop() directly    (m_head not atomic — single consumer)
```

There is no intermediate channel. The ring IS the queue. The consumer pops from
`MpscRing` directly; the wake lambda's only job is to release `m_recv` to unblock
the consumer.

### Coordinator invariant

`m_recv` is held ↔ ring is empty. This mirrors `RecvChannel`'s `m_recv` invariant
exactly, but the buffer it guards is the MPSC ring rather than a cooperator-private
array.

- Producer pushes → wake lambda releases `m_recv` → blocked consumer wakes, pops.
- Consumer pops last item → re-acquires `m_recv` immediately (not held → fast).
- Wake lambda clears `m_wakePending` with `seq_cst` then re-checks `IsEmpty()` to
  close the lost-wakeup window (producer may push between last Pop and the store).

### Why not a FixedChannel as the consumer-side buffer?

The original design used `MutexRing → drain context → FixedChannel → consumer`.
The drain context existed only to forward items from one queue to the other.
Eliminating it:
- Removes one full queue stage (halves buffer memory for the same logical capacity).
- Removes the drain context spawn per batch (one cooperator stack per wake).
- Makes the wake lambda O(1) instead of O(N).
- Removes the mutex on the producer hot path (replaced by wait-free CAS).

---

## Passage performance

### The real bottleneck

Throughput in benchmarks is ~12–13k items/s (~80μs/batch). This is entirely
determined by the Submit → CQE delivery round-trip:

```
producer write() to eventfd
  → kernel queues io_uring CQE for SubmissionDrainer's pending io::Read
  → cooperator's Poll() picks it up
  → SubmissionDrainer context unblocks, runs DrainSubmissions()
  → wake lambda spawned as new context
  → wake lambda runs, releases m_recv
  → consumer unblocks, pops items
```

The ring implementation (wait-free vs mutex), the size of the wake lambda, and the
number of items batched per wake have essentially no effect on this number. All
optimisations to the ring or wake lambda are free wins on code complexity and CPU
efficiency, but do not move the throughput ceiling.

### The yield loop under load

`Recv()` yields up to `kYieldThreshold` times before falling back to
`CoordinateWithKill` with a timeout. The intent is to give the cooperator's inner
loop a chance to call `Poll()` and process the wake CQE before committing to a
timeout SQE.

**Benchmark / unloaded cooperator**: the consumer is often the only context in the
yielded list. Each yield resumes in ~30ns, so 8 yields = ~240ns total — far less
than the ~80μs CQE latency. The yield loop is exhausted before the CQE arrives and
we fall immediately into `CoordinateWithKill`. The yield loop appears useless.

**Production / loaded cooperator**: the cooperator has many other contexts. Each
yield puts the consumer at the back of a long run queue; by the time it resumes,
the cooperator has called `Poll()` dozens of times across many context resumes. The
CQE is almost certainly processed within 1–3 yields, and `CoordinateWithKill` is
rarely reached. The adaptive timeout (`m_recvTimeoutUs`) stays at its initial value.

The benchmark is therefore a pessimistic case. Under real workloads the yield loop
is the common path, not overhead.

## SpscPassage

`SpscPassage<T,N>` reuses the same wake/coordinator flow and API as `Passage<T,N>`,
but swaps in a single-producer/single-consumer ring:

- Producer side removes CAS on `m_tail` (single writer).
- Consumer behavior and shutdown semantics are identical to `Passage`.
- In debug builds, push asserts if multiple producer threads attempt to use the ring.

Use `SpscPassage` when producer cardinality is known to be one; use `Passage` for MPSC.

### Benchmark interpretation

`BM_Passage_Throughput/N` — 1 producer, consumer in a tight loop, ring capacity 256.
The `N` argument is the batch size per benchmark iteration, not a meaningful variable
for throughput (throughput is flat across all batch sizes at ~12.5k items/s because
it measures items/second, not iterations/second, and each batch costs one CQE
round-trip regardless of N).

`BM_Passage_NProducers/N` — N producers, fixed batch 1024, ring 512. Shows that the
mutex-free MPSC ring scales: 1 producer → ~25k/s, 2+ producers → ~51k/s (two
concurrent senders overlap their pushes, amortising the single CQE delivery slot).
Plateau at 2 producers confirms the drain pipeline is the bottleneck, not producer
mutex contention (which no longer exists).
