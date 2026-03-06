# coop/epoch/ — Epoch-Based Reclamation

Epoch-based reclamation (EBR) for shared, user-space objects accessed across contexts.
Provides safe deferred freeing when one context removes an object from a shared data structure
that another context may still be traversing.

## Core Problem

A context holds a pointer to a list node obtained from a shared structure, then yields.
Another context removes and frees that node. When the first context resumes, the pointer is
stale. EBR defers the free until all potential readers have passed a quiescent boundary.

## Design

### Epoch counter

`Manager::m_current` is a monotonically increasing `uint64_t` owned by the cooperator.
Zero is reserved as the sentinel for "not pinned." The epoch starts at 1. `Advance()` is
called by the writer to separate batches of retirements.

### Per-context pin state (`State`, `epoch.h`)

Each context carries a `State` in its `ContextVar` slot (file-scope `s_state` in `epoch.cpp`).
Two independent slots, each pinned at an epoch value or zero (unpinned):

- `traversal` — short-lived, managed by `Manager::Enter`/`Exit` or `Guard`. Held for the
  duration of a single data-structure traversal. Set to the current epoch on entry, cleared
  to zero on exit.

- `application` — long-lived, managed explicitly by the application. Held for the duration
  of a transaction or snapshot. Set to an epoch supplied by the caller (`Pin`), cleared by
  `Unpin`. Coop internals never touch this slot.

`SafeEpoch` scans all contexts on the cooperator and returns the minimum non-zero value
across both slots. If no context is pinned, it returns `UINT64_MAX` (everything reclaimable).

### Retire queue (`RetireEntry`, `Manager`)

`RetireEntry` is an intrusive singly-linked node embedded in the object being retired.
The `reclaim` function pointer receives the `RetireEntry*`; the caller casts back to the
concrete type and frees it however it was allocated (slab return, delete, etc.).

Entries are appended to a FIFO tail and consumed from the head. Because retirement always
stamps the current epoch, the queue is monotonically ordered by `m_retiredAt`. `Reclaim`
stops at the first entry that is still protected — no full scan needed.

Reclamation condition: `entry->m_retiredAt < SafeEpoch()`. Strict less-than means an entry
retired at epoch E is not freed while any context is pinned at epoch E — it may still be
reading an object that was live at E.

### Manager lifecycle

One `Manager` instance per cooperator, created and owned by the application (not the
cooperator itself). Registered on the cooperator thread via `SetManager` / `GetManager`
(thread-local pointer). The bootstrap task that starts per-cooperator services calls
`SetManager` before any epoch operations run.

`Manager` is policy-free: it provides `Retire` (enqueue), `SafeEpoch` (compute horizon),
and `Reclaim` (free below horizon). When and how often to call `Reclaim` is a consumer
decision — typically once per scheduler loop iteration or per commit.

### Guard (RAII traversal pin)

`Guard` pins the traversal slot on construction and unpins on destruction. Captures the
pinned epoch in `Epoch()` for consumers that need to pass it to lock-free operations.

```cpp
{
    epoch::Guard guard(manager);      // pins traversal
    auto* node = list.Find(key);      // safe to read node
}                                     // traversal unpinned
manager.Reclaim();                    // may now free node if retired
```

## Multi-cooperator support

`SafeEpoch` reads `m_epochWatermark` from every cooperator in the global registry via
`Cooperator::VisitRegistry`. Each watermark is an `std::atomic<Epoch>` stored in the
`Cooperator` struct itself, published by `Manager::PublishWatermark()`.

### Watermark publish protocol

`PublishWatermark()` is called after every pin/unpin (`Enter`, `Exit`, `Pin`, `Unpin`):

1. Scan all contexts on the local cooperator — `State` fields are non-atomic (same thread).
2. Compute `min(traversal, application)` across all contexts, defaulting to `Alive()`.
3. `release`-store the result into `Cooperator::m_epochWatermark`.

Since only the cooperator's own thread ever writes the watermark, no CAS is needed —
a plain `store(release)` is sufficient. Other threads `load(acquire)` it in `SafeEpoch`.

The publish happens before the Coordinator or semaphore signal that lets a remote writer
proceed, establishing the happens-before chain:

```
B: watermark.store(E, release)
B: signal to A (semaphore.release / coordinator.Release)
A: signal from B (semaphore.acquire / coordinator.Acquire)
A: watermark.load(acquire)  → sees E
```

### SafeEpoch

O(cooperators) atomic reads. Returns the global minimum across all cooperator watermarks.
A cooperator whose Manager has been destroyed resets its watermark to `Alive()` in `~Manager`.

### State fields

`State::traversal` and `State::application` remain plain (non-atomic) `Epoch` values.
They are read and written only on the owning cooperator's thread. The watermark atom is
the only cross-thread visibility boundary.

### Cross-cooperator Coordinator limitation

`Coordinator` manipulates a single cooperator's context queues (blocked → yielded). It is
not safe to use across cooperators. Use `Cooperator::Submit` or OS primitives
(`std::binary_semaphore`) for cross-cooperator synchronization.
