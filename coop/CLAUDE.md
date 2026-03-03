# coop/ — Core Internals

Deep implementation details for the scheduler, context lifecycle, stack pool, and coordination
fast paths. For API surface and usage patterns, see the top-level `CLAUDE.md`.

## StackPool (`stack_pool.h`)

Size-class allocator for context stack segments, owned by `Cooperator`. Eliminates
`mmap`/`munmap` (debug) or `malloc`/`free` (release) syscalls on the spawn/exit hot path.

**Buckets**: 6 power-of-2 sizes from 4KB to 128KB. Requests are rounded up via
`RoundUpStackSize()`. Allocations larger than 128KB bypass the pool. Each bucket caches up to
32 freed segments via an intrusive `FreeNode*` overlay on dead memory.

**Allocation path**: `Spawn`/`Launch` call `m_stackPool.Allocate(roundedSize)` — checks the
free list first, falls back to `RawAllocate` (mmap+guard pages in debug, malloc in release).
`HandleCooperatorResumption(EXITED)` calls `m_stackPool.Free(ptr, size)` — returns to the
free list (evicting oldest on cap), or `RawFree` if the bucket is full.

Stack sizes in `SpawnConfiguration` are transparently rounded up, so contexts may get slightly
more stack than requested (strictly better).

## CoordinateWith / CoordinateWithKill Fast Paths

**Single-arg fast path**: `CoordinateWithImpl` bypasses `MultiCoordinator` for a single argument,
using `Acquire` directly. This generates much smaller code that the compiler inlines.

**Two-pass TryAcquire**: `MultiCoordinator::Acquire` tries all coordinators without hooking up
to wait lists first. Since scheduling is cooperative, no coordinator state can change between
TryAcquire calls. Only if all fail does it hook up and block. This eliminates
`AddAsBlocked`/`RemoveAsBlocked` on the uncontended fast path (~60% of cycles previously).

**CoordinateWithKill fast path**: for the common case of a single `Coordinator*` argument
(the IO hot path — `Handle::Wait`), `CoordinateWithKill` has a `constexpr if` specialization
that checks `IsKilled()` then `TryAcquire()` before falling through to the full
`CoordinateWith` path. This avoids constructing a 2-arg `MultiCoordinator` (kill signal +
coordinator) on every uncontended IO wait. Safety: cooperative scheduling means nothing changes
between the two checks; if TryAcquire fails, the full multiplexed path handles it correctly.

## Scheduler Internals (`cooperator.cpp`)

Contexts have three states: `YIELDED` (runnable), `RUNNING` (active on the cooperator's thread),
and `BLOCKED` (waiting for a coordinator release, e.g. IO completion or lock acquisition).

The cooperator loop in `Cooperator::Launch()`:
```
while (has yielded contexts OR not shutdown OR shutdown kill not done):
    1. If shutdown requested and kill not done: spawn kill context (kills all others)
    2. If yielded list empty:
       a. Poll io_uring (may move blocked -> yielded)
       b. If still empty and blocked exist: try SpawnSubmitted(false), continue
       c. If nothing at all: SpawnSubmitted(true) blocks waiting for external Submit()
    3. Pop up to 16 yielded contexts, Resume each one:
       - After each Resume returns (context yielded/blocked/exited): poll io_uring
       - This interleaved polling is critical -- CQE processing between context
         resumes is what unblocks Handle::Flash barriers during destruction
```

**Shutdown sequence**: `Shutdown()` sets `m_shutdown` flag and wakes the submission semaphore.
The loop spawns a temporary kill context that visits all live contexts and fires their kill
signals (`schedule=false` -> moved to yielded, not immediately switched to). Handlers that
loop (accept loops, read loops) check `IsKilled()` and break. Handlers that want kill-aware
IO use `CoordinateWithKill` explicitly. Handle destructors run Cancel + Flash during stack
unwind, draining in-flight IO.

**Important**: the loop condition includes `!shutdownKillDone` (guarantees the kill logic runs
even when all contexts are blocked) and `m_uring.PendingOps() > 0` (keeps the loop alive to
poll io_uring for cancel CQEs while Handle destructors drain in-flight operations).

## Context Lifecycle (`context.cpp`)

**Construction**: parent registers child in `m_children` list; first child `TryAcquire`s the
parent's `m_lastChild` coordinator (holds it so the parent's destructor will block).

**Kill propagation**: `Kill(other)` uses iterative post-order traversal (children before parents)
to avoid stack overflow on deep trees. For each descendant: fires `m_killedSignal` with
`schedule=false` (no context switches during traversal). For the target itself, fires with
the caller's `schedule` flag. Contexts blocked on coordinators are woken via `CoordinateWithKill`'s
kill signal multiplexing when they are next scheduled.

**Destruction** (runs on the context's own stack, in `CoopContextEntry`):
1. `Detach()` from parent — removes from children list; last child `Release`s `m_lastChild`
2. `Kill(this)` if not already killed — propagates to any remaining children
3. Clear `m_handle->m_context` pointer
4. `m_lastChild.Acquire(this)` — blocks until all children have exited and detached
5. Remove from cooperator's `m_contexts` list

After `~Context()`, `CoopContextEntry` does `ContextSwitch(EXITED)` back to the cooperator,
which returns the allocation to the `StackPool` for reuse (or frees it if the pool bucket is
full).

**Launchable note**: instances are placement-new'd onto the context's stack segment. Their C++
destructors run via `m_cleanup` (a typed destructor trampoline set by `Launch<T>`) after
`m_entry` returns but before `~Context()`. This allows Launchable members (Descriptor,
PlaintextStream, etc.) to use RAII normally — their destructors may do cooperative IO.
