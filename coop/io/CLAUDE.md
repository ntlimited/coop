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
`m_coord->Release(ctx, false)` (unblocks whoever is waiting on the coordinator). Neither
completion handler advances the CQ head — they only read `cqe->res`. The kernel-visible head is
advanced once per drain by `Uring::Poll` (see "CQ reaping" below), not once per CQE.

**Blocking pattern** (`Wait()` / `WaitKill()`): `Wait()` calls `CoordinateWith(ctx, m_coord)`
which blocks until the Handle's coordinator is released (IO completes). It is intentionally not
kill-aware, allowing post-kill cleanup IO. `WaitKill()` uses `CoordinateWithKill` instead and
returns `-ECANCELED` when kill wins. The operation macros generate both plain blocking wrappers
(`Recv`, `Accept`, ...) and kill-aware blocking wrappers (`RecvKill`, `AcceptKill`, ...).
`Result()` provides non-blocking access to the cached result (asserts all CQEs are drained).

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

**CQ reaping**: after submitting, `Poll()` reaps the whole ready batch with `io_uring_for_each_cqe`
and then advances the kernel-visible CQ head exactly once via `io_uring_cq_advance(n)`. The
alternative — `io_uring_cqe_seen` inside each completion handler — issues a release store to the CQ
head per CQE plus pays the terminating empty-queue probe of `io_uring_peek_cqe` on every Poll. The
batched form is safe because each callback only reads `cqe->res` and never depends on the kernel
reclaiming a slot mid-drain. The win scales with how many completions land in one window: a Poll
that reaps a single CQE is one head store either way (and still drops the peek probe), while a
fan-out burst that reaps a large batch collapses N head stores into one. Completions that overflow
the ring stay on the kernel overflow list and surface on the next Poll — the scheduler polls
continuously, so nothing is lost, only deferred one iteration.

**Non-native urings** (running as dedicated contexts via `Uring::Run()`) use the same deferred
model — their `Poll()` submits + processes CQEs on each scheduling round. IO latency is bounded
by the round-robin cycle time.

`Init()` uses a progressive fallback chain: `DEFER_TASKRUN` -> `COOP_TASKRUN` -> `SINGLE_ISSUER`
only -> bare init. Each step logs a warning.

**Registered ring fd**: after a successful init, `Init()` calls `io_uring_register_ring_fd()`.
`io_uring_enter()` is the hottest syscall coop makes, and coop runs many cooperators in one
process with one ring per thread — so the process file table is shared (CLONE_FILES) and the
kernel makes every enter pay an atomic fd refcount grab/put plus an fd->file lookup on the ring
fd. Registering the ring resolves the file once and lets each enter reference it by a small index
(`IORING_ENTER_REGISTERED_RING`), skipping that per-enter tax. liburing flips the ring into
registered-fd mode on success and ORs the flag into every later enter, so Submit/Poll/WaitAndPoll
need no change. Safe because `SINGLE_ISSUER` already guarantees only the registering (owning)
thread ever enters the ring — the one documented caveat (register on one thread, enter on
another) cannot arise. Needs kernel 5.18+; on older kernels the register call fails, the ring
keeps its plain fd, and `Init()` warns and continues. Measured saving: ~15ns per enter in an
isolated multi-thread microbench (~12-15%), ~8-20ns per blocking `io::Read` round trip end to end
in coop (the enter is one part of a ~280ns scheduler round trip, so a smaller fraction there).

**SQPOLL** (`sqpoll`): `IORING_SETUP_SQPOLL` — a kernel thread polls the SQ ring for new
entries, eliminating `io_uring_enter()` syscalls for submission. Incompatible with
`coopTaskrun`/`deferTaskrun` — disable those when enabling SQPOLL. Requires `CAP_SYS_ADMIN`
(or cgroup permissions) and a larger ring (1024+ entries) to avoid SQ overflow under
concurrent load. `sqpollIdleMs` controls how long the kernel thread busy-polls before going
idle (0 = kernel default ~1000ms). Under sustained load the idle timeout is irrelevant; tune
for bursty workloads.

**Benchmark finding**: SQPOLL yields 44-70% throughput improvement under concurrent load
(345K req/s vs 203K baseline at 256 connections). The win scales with concurrency because the
kernel thread amortizes submission cost across more in-flight operations. At low concurrency
(10 connections) the improvement is ~32%. The default `COOP_TASKRUN` mode is flat at ~200K
regardless of concurrency. See `bench_server.cpp --sqpoll` for testing.

## IO Operation Macros (`detail/op_macros.h`)

Two macro sets for generating the 4 standard operation variants:

- `COOP_IO_IMPLEMENTATIONS(name, prep_fn, ARGS)` — standard path, blocking variants go
  through io_uring unconditionally.
- `COOP_IO_IMPLEMENTATIONS_FASTPATH(name, prep_fn, try_fn, ARGS)` — blocking variants try
  a nonblocking direct syscall before constructing Handle/Coordinator/SQE machinery. On
  success or hard error, returns immediately. On EAGAIN/EWOULDBLOCK, falls through to io_uring.

**Fast path trade-off**: saves ~500ns per operation when data is immediately available (skips
Handle construction, Coordinator TryAcquire, SQE allocation, Poll, Finalize). Costs a wasted
syscall when data is not available (EAGAIN). Currently used by Recv and Send. Under sustained
concurrent load, recv often EAGAINs (request hasn't arrived yet), adding a syscall per request
for no benefit. The fast path primarily helps latency in the pre-staged data case (keep-alive
connections, buffered sends).

**Adding fast path to new operations**: write a static inline `Try*` wrapper that calls the
direct syscall with appropriate nonblocking flags (e.g. `MSG_DONTWAIT` for socket ops), then
use `COOP_IO_IMPLEMENTATIONS_FASTPATH`. The Try function signature must match
`int try_fn(int fd, ...args...)` using standard syscall return conventions.

Both macro families now also generate a kill-aware blocking family named `*Kill` (`RecvKill`,
`PollKill`, `AcceptKill`, etc.). These wrappers preserve the default blocking API while making
kill sensitivity explicit and uniform across the operation surface.

## Socket Shutdown Guard (`shutdown_on_kill.h`)

`ShutdownOnKillGuard` is the amortized alternative for tight socket loops that do not want to
plumb `*Kill` through every call. It spawns one detached watcher per guarded descriptor; the
watcher waits on the owning context's kill signal and then issues `Shutdown(desc, SHUT_RDWR)` to
wake plain blocking socket IO. This is intentionally a socket-level wake strategy, not a generic
replacement for per-call kill-aware IO.

## Sendfile (`sendfile.h`)

Sends file data directly to a socket via the `sendfile()` syscall — zero userspace copies. Uses
`io::Poll` fallback on EAGAIN (socket must be non-blocking). `SendfileKill` /
`SendfileAllKill` are the explicit kill-aware siblings for loops that want prompt cancellation
without a socket-level shutdown guard.
```cpp
int Sendfile(Descriptor& desc, int in_fd, off_t offset, size_t count);
int SendfileAll(Descriptor& desc, int in_fd, off_t offset, size_t count);
int SendfileKill(Descriptor& desc, int in_fd, off_t offset, size_t count);
int SendfileAllKill(Descriptor& desc, int in_fd, off_t offset, size_t count);
```
The SSL layer provides `ssl::Sendfile` / `ssl::SendfileAll` which dispatch to `io::Sendfile` for
kTLS connections (kernel encrypts file data in-flight, zero copies) and fall back to pread+Send for
non-kTLS connections. Both layers now also expose explicit `*Kill` variants. The HTTP server keeps
the cheap default send paths and relies on `ShutdownOnKillGuard` in its connection handlers to wake
promptly during shutdown.

## Splice (`splice.h`)

Moves data between two sockets via a kernel pipe — zero userspace copies. Uses `io::Poll` for
cooperative waiting on both sides. `SpliceKill` is the explicit kill-aware sibling for loops that
want per-call cancellation rather than a socket-level shutdown guard. The caller manages the pipe
(`pipe2(pipefd, O_NONBLOCK)`, reuse across calls). Both sockets must be non-blocking.
```cpp
int pipefd[2];
pipe2(pipefd, O_NONBLOCK);
int n = io::Splice(in, out, pipefd, 65536);  // up to 65KB per call
int k = io::SpliceKill(in, out, pipefd, 65536);
```
The TCP proxy uses this for bidirectional relay — data moves between client and upstream sockets
entirely in-kernel without entering userspace. The example now pairs plain `Splice` with
`ShutdownOnKillGuard` to keep the hot relay loop on the cheap blocking path while still waking
promptly on kill.
