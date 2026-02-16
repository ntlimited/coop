# Project: coop

## Overview

The `coop` project is a low level library for cooperative multitasking in C++. It prioritizes:
* High performance that trusts developers to know what they are doing
* Opinionated abstractions
* Safety, but not at the expense of performance of the above

## Code Organization
```
coop/           - Source files (.cpp)
build/debug/    - Debug build artifacts (git-ignored)
build/release/  - Release build artifacts (git-ignored)
tests/          - GTest code used to validate library behavior
benchmarks/     - Benchmark code to evaluate performance of the library and of alternative idioms
```

## Coding Standards

### Style
- 4-space indentation, no tabs
- Max line length: 100 characters, but sanity prevails
- `PascalCase` is used for classes, methods
- Member variables are prefixed with `m_`
- `NULL` should never be used, use `nullptr` instead
- Comments should be used liberally to document wider intent and goals, but code should speak for itself
- Multiline comments should be avoided. Comment blocks should have a trailing, empty `//` line to
  improve visual separation

### Best Practices
- Patterns that stack allocate, or enable stack allocation, are preferred
- RAII should be used wherever possible
- Thread locals are explicitly disallowed outside of `::coop::Cooperator__thread_cooperator`
- stdint types (`int64_t`, `uint8_t`) are encouraged over `int`, `unsigned char` where possible/
  appropriate
- All I/O calls should be implemented through the coop::io patterns. Syscalls likely should as well.
- Use separate `build/release` and `build/debug` directories for release and debug mode builds
  respectively
- Whenever context needs to be gathered at sufficient cost, suggest changes for CLAUDE.md directives
  that will reduce future costs.

### Error Handling
- Errors should bubble up using a checked-return pattern

## Architecture Notes

The library revolves around `Cooperator` and `Context` instances. The latter provide the stacks
which code runs on, and the former handles cooperating between them. The most fundamental building
block that ties these together is the `Coordinator`, which functions as a mutex facsimile and allows
one `Context` to block on action by another.

All actual "blocking" operations are forbidden, ideally including syscalls which should be dispatched
via the `coop::io` system which is built on top of `io_uring`.

### Context (`coop/context.h`)
Contexts are the unit of execution. Each has its own stack segment allocated by the Cooperator.
```cpp
ctx->SetName("name");
ctx->Yield();           // cooperate — return control to scheduler
ctx->IsKilled();        // check kill signal
ctx->GetCooperator();   // owning cooperator
ctx->Parent();          // parent context (tree structure)
ctx->Detach();          // disassociate from parent
```
Contexts form a parent-child tree. Killing a parent kills its children.

**Naming note**: `Context::Handle` is a lifecycle handle to a spawned context (for kill/tracking).
`io::Handle` is an io_uring operation handle (for async IO). They are unrelated.

### Free Functions (`coop/self.h`, `coop/cooperator.hpp`)
Thread-local convenience functions that avoid explicit cooperator/context references:
```cpp
Context* Self();              // current context
bool Yield();                 // cooperate
bool IsKilled();              // check kill signal
bool IsShuttingDown();        // check cooperator shutdown
Cooperator* GetCooperator();  // thread-local cooperator
io::Uring* GetUring();        // thread-local uring
```

`Spawn` and `Launch` are also available as free functions (template, in `cooperator.hpp`):
```cpp
Spawn([](Context* ctx) { ... });
Spawn(config, [](Context* ctx) { ... }, &handle);
Launch<MyHandler>(config, fd, ...);
Launch<MyHandler>(args...);
```

### StackPool (`coop/stack_pool.h`)
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

### Spawn vs Launch (`coop/cooperator.h`)
Two ways to create contexts:
- `bool Spawn(Fn const& fn)` — lambda copied to context stack, for simple one-off tasks
- `T* Launch<T>(Args&&...)` — Launchable subclass, forwarded ctor args, for stateful handlers

Both accept optional `SpawnConfiguration` and `Context::Handle*`.
Both are available as free functions (prefer these) or as `Cooperator` methods.

### Launchable (`coop/launchable.h`)
OOP alternative to lambda spawning. Subclass, implement `virtual void Launch() final`. Instance is
placement-new'd onto the context's stack segment.
```cpp
struct MyHandler : Launchable {
    MyHandler(Context* ctx, int fd, ...) : Launchable(ctx), m_fd(fd), ... {
        ctx->SetName("MyHandler");
    }
    virtual void Launch() final { /* runs on own context */ }
    io::Descriptor m_fd;
};
Launch<MyHandler>(fd, ...);
```

### Coordinator (`coop/coordinator.h`)
Mutex facsimile — one context holds, others block in a FIFO queue.
- `TryAcquire(ctx)` / `Acquire(ctx)` — non-blocking / blocking
- `Release(ctx)` — unblocks head of wait list
- `Flash(ctx)` — barrier: wait, acquire, release (serialization point)

### CoordinateWith / CoordinateWithKill (`coop/coordinate_with.h`)
`CoordinateWith` blocks the calling context until one of the given coordinators or signals is
released. Arguments may be `Coordinator*` or `Signal*` in any combination, with an optional
trailing `time::Interval` timeout. Signal arguments are converted internally and cleaned up
automatically. **CoordinateWith does NOT implicitly include the kill signal.**

`CoordinateWithKill` is sugar that prepends the context's kill signal as the first argument,
delegates to `CoordinateWith`, and translates index 0 (kill) to the `Killed()` sentinel.
```cpp
auto result = CoordinateWith(ctx, &coord);           // no kill awareness
auto result = CoordinateWithKill(ctx, &coord);       // kill-aware
auto result = CoordinateWith(ctx, &signal, &coord);  // explicit Signal*
auto result = CoordinateWith(ctx, &coord, timeout);  // with timeout
auto result = CoordinateWithKill(ctx, &coord, timeout); // kill + timeout
```
Both have `(Args...)` convenience overloads that use `Self()` as the context.

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

### CoordinationResult (`coop/coordination_result.h`)
Return type of `CoordinateWith` / `CoordinateWithKill`. Wraps the acquired `Coordinator*` with
sentinel detection:
- `Killed()` — only possible from `CoordinateWithKill`
- `TimedOut()` / `Error()` — check sentinel conditions
- `operator Coordinator*()` / `operator->()` — access the acquired coordinator
- `operator==(Coordinator*)` / `operator==(const Coordinator&)` — pointer comparison

No `operator size_t()`. The `index` field is internal to `MultiCoordinator` / `CoordinateWith`.

### Scheduler Internals (`coop/cooperator.cpp`)
Contexts have three states: `YIELDED` (runnable), `RUNNING` (active on the cooperator's thread),
and `BLOCKED` (waiting for a coordinator release, e.g. IO completion or lock acquisition).

The cooperator loop in `Cooperator::Launch()`:
```
while (has yielded contexts OR not shutdown OR shutdown kill not done):
    1. If shutdown requested and kill not done: spawn kill context (kills all others)
    2. If yielded list empty:
       a. Poll io_uring (may move blocked → yielded)
       b. If still empty and blocked exist: try SpawnSubmitted(false), continue
       c. If nothing at all: SpawnSubmitted(true) blocks waiting for external Submit()
    3. Pop up to 16 yielded contexts, Resume each one:
       - After each Resume returns (context yielded/blocked/exited): poll io_uring
       - This interleaved polling is critical — CQE processing between context
         resumes is what unblocks Handle::Flash barriers during destruction
```

**Shutdown sequence**: `Shutdown()` sets `m_shutdown` flag and wakes the submission semaphore.
The loop spawns a temporary kill context that visits all live contexts and fires their kill
signals (`schedule=false` → moved to yielded, not immediately switched to). All blocking IO is
kill-aware (via `CoordinateWithKill` in `Wait()`), so contexts blocked in `Recv`, `Accept`,
etc. wake up and return `-ECANCELED` when killed.

**Important**: the loop condition includes `!shutdownKillDone` (guarantees the kill logic runs
even when all contexts are blocked) and `m_uring.PendingOps() > 0` (keeps the loop alive to
poll io_uring for cancel CQEs while Handle destructors drain in-flight operations).

### Context Lifecycle (`coop/context.cpp`)
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

### IO Handle Lifecycle (`coop/io/handle.cpp`)
An `io::Handle` ties an io_uring SQE to a `Coordinator` for synchronization.

**Submission**: `Submit(sqe)` calls `m_coord->TryAcquire(m_context)` (holds the coordinator),
sets `m_pendingCqes = 1`, and pushes onto descriptor's handle list. The SQE is **not** submitted
to the kernel immediately — it remains in the SQ ring until `Uring::Poll()` calls
`io_uring_submit()` (deferred submission). `SubmitLinked` (for timeouts) sets `m_pendingCqes = 2`
and links a timeout SQE. All SQE acquisition goes through `Uring::GetSqe()` which self-corrects
on SQ ring exhaustion by flushing pending SQEs.

**Completion**: io_uring CQE arrives → `Callback` dispatches via tagged pointer (bit 0 of
userdata): untagged → `Complete()`, tagged → `OnSecondaryComplete()`. Both call `Finalize()`
which decrements `m_pendingCqes`; when it hits 0, pops from descriptor list and calls
`m_coord->Release(ctx, false)` (unblocks whoever is waiting on the coordinator).

**Blocking pattern** (`Wait()`): calls `CoordinateWithKill(ctx, m_coord)` which multiplexes
the Handle's coordinator with the context's kill signal. If the context is killed, returns
`-ECANCELED` immediately; otherwise returns `m_result`. `Result()` provides non-blocking access
to the cached result (asserts all CQEs are drained). This makes all blocking IO kill-aware.

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
the async op (which calls `Submit` → `TryAcquire` again). The losing coordinator was never
acquired (its ordinal was removed from the wait list), and the Handle's pending CQE will
eventually fire and release it via `Finalize`.

### Uring Configuration (`coop/io/uring_configuration.h`, `coop/io/uring.cpp`)
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
submit → block → wake-on-completion. With `COOP_TASKRUN`, task_work piggybacks on the submit
`io_uring_enter()` for free. With `DEFER_TASKRUN`, `Poll()` must make a separate
`io_uring_get_events()` syscall. See `bench_uring_config.cpp` for numbers.

**Deferred submission**: SQEs are not submitted to the kernel at `Handle::Submit()` time.
Instead, `Uring::Poll()` calls `io_uring_submit()` unconditionally before processing CQEs.
This batches multiple SQEs from a single context resume (or across resumes if poll frequency
is reduced) into one `io_uring_enter()` syscall. For `COOP_TASKRUN`, this also flushes pending task_work.
For `DEFER_TASKRUN`, `io_uring_get_events()` is additionally called since `io_uring_submit()`
alone cannot flush deferred completions. `Uring::GetSqe()` provides self-correcting SQE
acquisition: if the SQ ring is full, it flushes pending SQEs and retries.

`Poll()` checks `m_pendingSqes > 0 || (*m_ring.sq.kflags & IORING_SQ_TASKRUN)` before calling
`io_uring_submit()`. The `IORING_SQ_TASKRUN` flag (a volatile read from kernel-mapped SQ ring
memory) is set by the kernel when completions are pending under `COOP_TASKRUN` mode. When
neither SQEs nor task_work are pending — the common case on pure-yield resumes — the submit
is skipped entirely, saving ~9ns per Poll() (~3% of yield cost).

**Non-native urings** (running as dedicated contexts via `Uring::Run()`) use the same deferred
model — their `Poll()` submits + processes CQEs on each scheduling round. IO latency is bounded
by the round-robin cycle time.

`Init()` uses a progressive fallback chain: `DEFER_TASKRUN` → `COOP_TASKRUN` → `SINGLE_ISSUER`
only → bare init. Each step logs a warning.

### IO (`coop/io/`)
All IO goes through io_uring. Each operation has 4 variants:
1. `bool Op(Handle&, ...)` — async
2. `bool Op(Handle&, ..., time::Interval)` — async + timeout
3. `int Op(Descriptor&, ...)` — blocking (creates Coordinator internally)
4. `int Op(Descriptor&, ..., time::Interval)` — blocking + timeout

Key operations: `Accept`, `Recv`, `Send`, `Read`, `ReadFile`, `Open`, `Close`, `Shutdown`,
`Connect`, `Sendfile`, `Splice`

`Accept` args default to `nullptr, nullptr, 0` (addr, addrLen, flags) so existing `Accept(desc)`
call sites continue to work. Pass a `sockaddr*` and `socklen_t*` to retrieve peer addresses.

Blocking variant pattern:
```cpp
int Recv(Descriptor& desc, void* buf, size_t size) {
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Recv(handle, buf, size)) return -EAGAIN;
    return handle.Wait();  // blocks via CoordinateWithKill, returns result or -ECANCELED
}
```

**Connect** accepts either a dotted-quad IP or a hostname. Hostnames are resolved cooperatively
via `Resolve4` (DNS over UDP through io_uring).

**Resolve4** (`coop/io/resolve.h`) performs cooperative DNS resolution. Checks `/etc/hosts` first,
then queries nameservers from `/etc/resolv.conf` over UDP. IPv4 (A records) only.
```cpp
int Resolve4(const char* hostname, struct in_addr* result);
int Resolve4(const char* hostname, struct in_addr* result, time::Interval timeout);
```
Returns 0 on success, negative errno on failure (-ENOENT for NXDOMAIN, -ETIMEDOUT, -EAGAIN).
Numeric addresses are detected and parsed directly without DNS. Config files are parsed lazily
on first call and cached in file-static globals.

`PlaintextStream` wraps a `Descriptor` for socket IO (`Recv`, `Send`, `SendAll`).
`ReadFile(path, buf, bufSize)` reads an entire file, returns bytes read or negative errno.

**Sendfile** (`coop/io/sendfile.h`) sends file data directly to a socket via the `sendfile()`
syscall — zero userspace copies. Uses `io::Poll` fallback on EAGAIN (socket must be non-blocking).
```cpp
int Sendfile(Descriptor& desc, int in_fd, off_t offset, size_t count);
int SendfileAll(Descriptor& desc, int in_fd, off_t offset, size_t count);
```
The SSL layer provides `ssl::Sendfile` / `ssl::SendfileAll` which dispatch to `io::Sendfile` for
kTLS connections (kernel encrypts file data in-flight, zero copies) and fall back to pread+Send for
non-kTLS connections. The HTTP server uses `io::SendfileAll` for static file serving, eliminating
the previous 65KB staging buffer and multiple string copies.

**Splice** (`coop/io/splice.h`) moves data between two sockets via a kernel pipe — zero userspace
copies. Uses `io::Poll` for cooperative waiting on both sides. The caller manages the pipe (create
with `pipe2(pipefd, O_NONBLOCK)`, reuse across calls). Both sockets must be non-blocking.
```cpp
int pipefd[2];
pipe2(pipefd, O_NONBLOCK);
int n = io::Splice(in, out, pipefd, 65536);  // up to 65KB per call
```
The TCP proxy uses this for bidirectional relay — data moves between client and upstream sockets
entirely in-kernel without entering userspace.

### SSL/TLS (`coop/io/ssl/`)

Two BIO modes for TLS connections:

**Memory BIO** (default): OpenSSL uses memory BIOs decoupled from the socket. All I/O goes
through staging buffers: `SSL_write → wbio → FlushWrite → io::Send`. Works with any socket
type (TCP, AF_UNIX). Requires a caller-provided staging buffer (16KB recommended).
```cpp
ssl::Connection conn(sslCtx, desc, buffer, sizeof(buffer));
```

**Socket BIO** (`ssl::SocketBio` tag): OpenSSL operates on the real socket fd via `SSL_set_fd`.
The handshake uses `io::Poll` for cooperative waiting on WANT_READ/WANT_WRITE. After handshake,
if kTLS activates, `ssl::Send`/`ssl::Recv` bypass OpenSSL entirely — `io::Send`/`io::Recv` go
straight to the kernel which handles crypto transparently. No staging buffer needed.
```cpp
sslCtx.EnableKTLS();  // must be called before creating connections
ssl::Connection conn(sslCtx, desc, ssl::SocketBio{});
conn.Handshake();     // probes kTLS activation (sets m_ktlsTx, m_ktlsRx)
```

**kTLS activation requirements**: TCP socket (not AF_UNIX), `EnableKTLS()` on the ssl::Context,
kernel `tls` module loaded, and a cipher suite the kernel supports. TLS 1.3 typically gets TX
only; TLS 1.2 can get both TX+RX. Falls back gracefully — socket BIO without kTLS still works,
just uses `SSL_write`/`SSL_read` + `io::Poll` instead of the memory BIO staging path.

**TCP_NODELAY** is set automatically in the socket BIO constructor. This is critical: kTLS
frames each `write()` as a complete TLS record, and Nagle's algorithm delays sending small TCP
segments. The interaction is catastrophic — without `TCP_NODELAY`, kTLS is 500x slower than
plaintext on loopback at 16KB messages. With `TCP_NODELAY`, kTLS performs within ~10% of
memory BIO at 16KB (187us vs 169us in benchmarks).

**Data path dispatch** (in `ssl::Send` and `ssl::Recv`):
1. `m_ktlsTx`/`m_ktlsRx` true → `write()`/`read()` directly + `io::Poll` on EAGAIN
   (zero OpenSSL involvement; synchronous when socket buffer available)
2. `m_buffer == nullptr` (socket BIO, no kTLS) → `SSL_write`/`SSL_read` + `io::Poll`
3. `m_buffer != nullptr` (memory BIO) → existing `FlushWrite`/`FeedRead` path

Both kTLS and socket BIO paths use direct syscalls with `io::Poll` fallback, avoiding uring
for the common case where the syscall succeeds immediately. This is critical for throughput —
routing every send through `io::Send` (uring SQE/CQE) adds ~20us per-op overhead that
dominates the crypto savings kTLS provides.

### HTTP Server (`coop/http/`)
Route table maps paths to handlers:
```cpp
struct Route {
    const char* path;
    const char* contentType;
    std::string (*handler)(Cooperator*);
};
```

`RunServer` accepts connections in a loop, launches an `HttpConnection` (Launchable) per client.
GET-only, HTTP/1.0-style (Connection: close). Optional `searchPaths` parameter enables static file
serving as a fallback when no route matches.

`SpawnStatusServer(co, port, staticPath)` provides a JSON API at `/api/status` and serves the
dashboard from static files.

## Design Review

These are red flags that should trigger pushback **before implementation**, even when the proposal
is well-reasoned and the plan is detailed. A good idea with the wrong orientation is worse than no
change — the cost of implementing and reverting far exceeds the cost of a harder conversation
upfront.

### Cleanup paths are sacred
Destructor and RAII teardown paths must work unconditionally — including on killed contexts. Any
change that requires an escape hatch (e.g. `AcquireAlways`) for cleanup to function is a design
smell. If a proposed change breaks cleanup, the change is wrong, not the cleanup.

### Opt-in vs opt-out orientation
When a change inverts an opt-in pattern to opt-out (or vice versa), enumerate both sides before
implementing: how many call sites benefit from the new default vs. how many need the escape hatch?
If the escape hatch cases are harder to get right (destructors, error paths, less-tested code),
the original orientation is probably correct. A small number of well-placed opt-in call sites is
better than pervasive opt-out burden.

### Escape hatches signal wrong direction
If a "simplification" requires a new back-door API to preserve existing behavior for certain
callers, that's strong evidence the simplification is pointed the wrong way. The need for an
escape hatch means the change is adding complexity, not removing it — it's just moving the
complexity to harder-to-audit places.

### Complexity budget
If a change touches more than ~5 files, introduces new friend declarations or extension classes,
or discovers multiple crash bugs during implementation, stop and re-evaluate the premise. The
implementation difficulty is signal about the design, not just about the implementation.

# Debugging guidelines

- Tests should be run in debug mode; release tests are not unimportant but should never be run
  before debug tests pass.
- Targetted usage of the gtest filter for changes is encouraged to save time, but run all tests
  when called for - making foundational changes

## Debugging Strategy

- Running tests with GDB attached can save time, both for diagnosing potentially hanging tests
  and investigating crashes that would be lost/harder to investigate after the process is dead
