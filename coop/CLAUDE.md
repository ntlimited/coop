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
(for example explicit async-handle accept loops), `CoordinateWithKill` has a `constexpr if`
specialization that checks `IsKilled()` then `TryAcquire()` before falling through to the full
`CoordinateWith` path. This avoids constructing a 2-arg `MultiCoordinator` (kill signal +
coordinator) on every uncontended explicit kill-aware wait. Safety: cooperative scheduling means
nothing changes between the two checks; if TryAcquire fails, the full multiplexed path handles it
correctly.

## Scheduler Internals (`cooperator.cpp`)

Contexts have three states: `YIELDED` (runnable), `RUNNING` (active on the cooperator's thread),
and `BLOCKED` (waiting for a coordinator release, e.g. IO completion or lock acquisition).

The cooperator loop in `Cooperator::Launch()`:
```
while (has yielded contexts OR not shutdown OR shutdown kill not done):
    1. If shutdown requested and kill not done: drain submissions, spawn kill context
    2. If yielded list empty:
       a. Poll io_uring + opportunistically drain submissions
       b. If still no runnable contexts: WaitAndPoll() blocks until the next CQE
          (ordinary IO completion or the submission-drainer eventfd read)
    3. Drain submissions, then pop up to 16 yielded contexts, Resume each one:
       - After each Resume returns (context yielded/blocked/exited): poll io_uring
       - Break early if m_hasSubmissions is set (atomic flag, no syscall)
       - Interleaved polling is critical -- CQE processing between context
         resumes is what unblocks Handle::Flash barriers during destruction
```

**Submission system**: eventfd-based. External threads push `SubmissionEntry` nodes to an
intrusive FIFO (mutex-protected), then `write()` to the eventfd. A dedicated submission-drainer
context keeps a blocking eventfd read in flight through io_uring, so cross-thread submits wake
the scheduler as an ordinary CQE. The scheduler also opportunistically checks `m_hasSubmissions`
and drains the list directly on scheduler iterations.

**Shutdown sequence**: `Shutdown()` sets `m_shutdown` flag and writes to the eventfd.
The loop spawns a temporary kill context that visits all live contexts and fires their kill
signals (`schedule=false` -> moved to yielded, not immediately switched to). Handlers that
loop (accept loops, read loops) must use an explicit wake path — generated blocking `*Kill`
wrappers, explicit `CoordinateWithKill` composition, timeouts, or a socket shutdown guard.
Handle destructors run Cancel + Flash during stack unwind, draining in-flight IO.

**Important**: the loop condition includes `!shutdownKillDone` (guarantees the kill logic runs
even when all contexts are blocked) and `m_uring.PendingOps() > 0` (keeps the loop alive to
poll io_uring for cancel CQEs while Handle destructors drain in-flight operations).

**Context switch** (`detail/context_switch.{S,h,cpp}`): the asm core `_coop_switch_context`
saves all callee-saved registers and swaps the stack; `ContextSwitch` is a plain compiler-visible
call on both architectures. That is load-bearing: hiding the call inside inline asm (to delegate
register saves to a clobber list) breaks the compiler's red-zone analysis, blinds whole-program
codegen to the control transfer, and measures perf-neutral anyway — see
`docs/context_switch_core_01.md` before changing the model. `ContextInit` seeds a fresh stack with the core's save-area layout
(x86-64: abort, trampoline, `%rbp = Context*`, rbx/r12-r15 zeroed); the trampoline reads
`Context*` from `%rbp` (x86-64) / `x19` (aarch64).

**Direct-yield fastpath** (opt-in, `CooperatorConfiguration::directYield`): a plain `Yield`
normally trampolines through the loop (two switches). With the fastpath, a yield that finds another
runnable context switches straight into it (`Cooperator::YieldFrom`). To keep the interleaved poll
above from starving, the chain is bounded by `directYieldBudget`: the budget refills in `Resume`
(just after the loop polls) and decrements per direct yield; at zero, a yield falls back through the
loop. Off by default; default path unchanged.

The same direct-switch also applies to the **self-block** path (`Cooperator::Block`) — a contended
`Coordinator`'s block/wake is the bulk of a cooperative lock/sleep's cost (sleep round-trip 81→35ns,
lock cycle 90→60ns with directYield on). The block fastpath does **not** poll inline (the yield one
does): a self-block can be a teardown `Flash` whose wake a premature poll would lose, so it registers
in `m_blocked` first and lets the budget bring the loop's poll back.

**IO governor** (`ioPresentLimit`, default 8; 0 disables): the budget bound alone defers io_uring
polling, which under `COOP_TASKRUN` can leave a queued SQE unsubmitted (timer never arms) for
milliseconds. On a direct yield, when work is pending the governor enters the kernel **inline** — one
`Poll()` submits SQEs and reaps completions in a single `io_uring_enter`. Submissions are detected
every switch (`HasPendingSubmissions`, a field read); completions are sampled. Cuts timer-wake
lateness under spinners from ~9.6ms to ~115µs at zero quiet-ring cost. See
`docs/fast_context_switch_01.md`.

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
