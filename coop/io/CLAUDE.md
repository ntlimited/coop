# coop/io/ — IO Internals

Deep implementation details for io_uring Handle lifecycle, uring configuration, and zero-copy
operations. For the IO operation API surface and variant table, see the top-level `CLAUDE.md`.

## IO Handle Lifecycle (`handle.cpp`)

An `io::Handle` ties an io_uring SQE to a `Coordinator` for synchronization.

**Submission**: `Submit(sqe)` calls `m_coord->TryAcquire(m_context)` (holds the coordinator),
sets `m_pendingCqes = 1`, and pushes onto descriptor's handle list. The SQE is **not** submitted
to the kernel immediately — it remains in the SQ ring until `Uring::Poll()` calls
`io_uring_submit()` (deferred submission). `SubmitLinked` (for timeouts) sets `m_pendingCqes = 2`
and links a timeout SQE. All SQE acquisition goes through `Uring::GetSqe()` which self-corrects
on SQ ring exhaustion by flushing pending SQEs.

**Completion**: io_uring CQE arrives -> `Callback` dispatches via tagged pointer (bit 0 of
userdata): untagged -> `Complete()`, tagged -> `OnSecondaryComplete()`. Both call `Finalize()`
which decrements `m_pendingCqes`; when it hits 0, pops from descriptor list and calls
`m_coord->Release(ctx, false)` (unblocks whoever is waiting on the coordinator).

**Blocking pattern** (`Wait()`): calls `CoordinateWith(ctx, m_coord)` which blocks until the
Handle's coordinator is released (IO completes). Returns `m_result`. Not kill-aware — blocking
IO continues to completion regardless of kill state, allowing post-kill cleanup IO. Callers
that want kill-aware IO use `CoordinateWithKill` explicitly with async handles. `Result()`
provides non-blocking access to the cached result (asserts all CQEs are drained).

**Encapsulation**: Handle fields are private. Internal access from IO operation macros and
implementation files goes through `detail::HandleExtension` (friend struct), which exposes
`GetSqe`, `Fd`, and `Timeout` static methods.

**PendingOps tracking**: `Submit`/`SubmitLinked` increment `m_ring->m_pendingOps`; `Finalize`
decrements it when `m_pendingCqes` reaches 0. The cooperator loop uses this counter to keep
polling io_uring during shutdown while cancel CQEs are still in flight.

**Destruction**: if `m_pendingCqes > 0`, submits `IORING_OP_ASYNC_CANCEL` (increments
`m_pendingCqes` for the cancel CQE), then `m_coord->Flash(ctx)` to block until all CQEs drain.
This means Handle destructors **cooperatively block** — the scheduler runs other contexts and
polls io_uring during the Flash, which is how the cancel CQEs get processed.

**Reuse after CoordinateWith(Kill)**: when `CoordinateWith` or `CoordinateWithKill` returns,
the winning coordinator was acquired by `MultiCoordinator`. Release it explicitly, then resubmit
the async op (which calls `Submit` -> `TryAcquire` again). The losing coordinator was never
acquired (its ordinal was removed from the wait list), and the Handle's pending CQE will
eventually fire and release it via `Finalize`.

## Uring Configuration (`uring_configuration.h`, `uring.cpp`)

`UringConfiguration` controls io_uring setup flags. Key fields:
- `coopTaskrun` (default **true**): `IORING_SETUP_COOP_TASKRUN` — defers kernel task_work to
  the next `io_uring_enter()`. Natural fit: task_work runs during submission, so completions
  are in the CQ by the time we peek. ~5% faster than bare at scale, lower variance.
- `deferTaskrun`: `IORING_SETUP_DEFER_TASKRUN` — stronger variant, completions only appear
  after explicit `io_uring_get_events()`. Gives full control but **adds ~20-30% overhead** in
  coop's submit-then-wait pattern (extra kernel transition per Poll). Lower latency variance.
  Use only when predictable completion timing matters more than throughput.
- `SINGLE_ISSUER` is always forced (not configurable).

**Benchmark finding**: `DEFER_TASKRUN` is consistently slower because coop's hot path is
submit -> block -> wake-on-completion. With `COOP_TASKRUN`, task_work piggybacks on the submit
`io_uring_enter()` for free. With `DEFER_TASKRUN`, `Poll()` must make a separate
`io_uring_get_events()` syscall. See `bench_uring_config.cpp` for numbers.

**Deferred submission**: SQEs are not submitted to the kernel at `Handle::Submit()` time.
Instead, `Uring::Poll()` calls `io_uring_submit()` unconditionally before processing CQEs.
This batches multiple SQEs from a single context resume (or across resumes if poll frequency
is reduced) into one `io_uring_enter()` syscall. For `COOP_TASKRUN`, this also flushes pending
task_work. For `DEFER_TASKRUN`, `io_uring_get_events()` is additionally called since
`io_uring_submit()` alone cannot flush deferred completions. `Uring::GetSqe()` provides
self-correcting SQE acquisition: if the SQ ring is full, it flushes pending SQEs and retries.

`Poll()` checks `m_pendingSqes > 0 || (*m_ring.sq.kflags & IORING_SQ_TASKRUN)` before calling
`io_uring_submit()`. The `IORING_SQ_TASKRUN` flag (a volatile read from kernel-mapped SQ ring
memory) is set by the kernel when completions are pending under `COOP_TASKRUN` mode. When
neither SQEs nor task_work are pending — the common case on pure-yield resumes — the submit
is skipped entirely, saving ~9ns per Poll() (~3% of yield cost).

**Non-native urings** (running as dedicated contexts via `Uring::Run()`) use the same deferred
model — their `Poll()` submits + processes CQEs on each scheduling round. IO latency is bounded
by the round-robin cycle time.

`Init()` uses a progressive fallback chain: `DEFER_TASKRUN` -> `COOP_TASKRUN` -> `SINGLE_ISSUER`
only -> bare init. Each step logs a warning.

## Sendfile (`sendfile.h`)

Sends file data directly to a socket via the `sendfile()` syscall — zero userspace copies. Uses
`io::Poll` fallback on EAGAIN (socket must be non-blocking).
```cpp
int Sendfile(Descriptor& desc, int in_fd, off_t offset, size_t count);
int SendfileAll(Descriptor& desc, int in_fd, off_t offset, size_t count);
```
The SSL layer provides `ssl::Sendfile` / `ssl::SendfileAll` which dispatch to `io::Sendfile` for
kTLS connections (kernel encrypts file data in-flight, zero copies) and fall back to pread+Send for
non-kTLS connections. The HTTP server uses `io::SendfileAll` for static file serving, eliminating
the previous 65KB staging buffer and multiple string copies.

## Splice (`splice.h`)

Moves data between two sockets via a kernel pipe — zero userspace copies. Uses `io::Poll` for
cooperative waiting on both sides. The caller manages the pipe (create with
`pipe2(pipefd, O_NONBLOCK)`, reuse across calls). Both sockets must be non-blocking.
```cpp
int pipefd[2];
pipe2(pipefd, O_NONBLOCK);
int n = io::Splice(in, out, pipefd, 65536);  // up to 65KB per call
```
The TCP proxy uses this for bidirectional relay — data moves between client and upstream sockets
entirely in-kernel without entering userspace.
