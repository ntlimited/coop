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

### CoordinationResult (`coop/coordination_result.h`)
Return type of `CoordinateWith`. Wraps the acquired `Coordinator*` with sentinel detection:
- `Killed()` / `TimedOut()` / `Error()` — check sentinel conditions
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
kill-aware (via `CoordinateWith` in `Wait()`), so contexts blocked in `Recv`, `Accept`,
etc. wake up and return `-ECANCELED` when killed.

**Important**: the loop condition includes `!shutdownKillDone` (guarantees the kill logic runs
even when all contexts are blocked) and `m_uring.PendingOps() > 0` (keeps the loop alive to
poll io_uring for cancel CQEs while Handle destructors drain in-flight operations).

### Context Lifecycle (`coop/context.cpp`)
**Construction**: parent registers child in `m_children` list; first child `TryAcquire`s the
parent's `m_lastChild` coordinator (holds it so the parent's destructor will block).

**Kill propagation**: `Kill(other)` fires the target's `m_killedSignal` then recursively kills
all children. Signal::Notify unblocks all waiters on the kill signal coordinator.

**Destruction** (runs on the context's own stack, in `CoopContextEntry`):
1. `Detach()` from parent — removes from children list; last child `Release`s `m_lastChild`
2. `Kill(this)` if not already killed — propagates to any remaining children
3. Clear `m_handle->m_context` pointer
4. `m_lastChild.Acquire(this)` — blocks until all children have exited and detached
5. Remove from cooperator's `m_contexts` list

After `~Context()`, `CoopContextEntry` does `ContextSwitch(EXITED)` back to the cooperator,
which calls `free()` on the context's allocation.

**Launchable note**: instances are placement-new'd onto the context's stack segment. Their C++
destructors run via `m_cleanup` (a typed destructor trampoline set by `Launch<T>`) after
`m_entry` returns but before `~Context()`. This allows Launchable members (Descriptor,
PlaintextStream, etc.) to use RAII normally — their destructors may do cooperative IO.

### IO Handle Lifecycle (`coop/io/handle.cpp`)
An `io::Handle` ties an io_uring SQE to a `Coordinator` for synchronization.

**Submission**: `Submit(sqe)` calls `m_coord->TryAcquire(m_context)` (holds the coordinator),
sets `m_pendingCqes = 1`, pushes onto descriptor's handle list, and submits to io_uring.
`SubmitLinked` (for timeouts) sets `m_pendingCqes = 2` and links a timeout SQE.

**Completion**: io_uring CQE arrives → `Callback` dispatches via tagged pointer (bit 0 of
userdata): untagged → `Complete()`, tagged → `OnSecondaryComplete()`. Both call `Finalize()`
which decrements `m_pendingCqes`; when it hits 0, pops from descriptor list and calls
`m_coord->Release(ctx, false)` (unblocks whoever is waiting on the coordinator).

**Blocking pattern** (`Wait()`): calls `CoordinateWith(ctx, m_coord)` which multiplexes
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

**Reuse after CoordinateWith**: when `CoordinateWith` returns, the winning coordinator was
acquired by `MultiCoordinator`. Release it explicitly, then resubmit the async op (which calls
`Submit` → `TryAcquire` again). The losing coordinator was never acquired by CoordinateWith
(its ordinal was removed from the wait list), and the Handle's pending CQE will eventually
fire and release it via `Finalize`.

### IO (`coop/io/`)
All IO goes through io_uring. Each operation has 4 variants:
1. `bool Op(Handle&, ...)` — async
2. `bool Op(Handle&, ..., time::Interval)` — async + timeout
3. `int Op(Descriptor&, ...)` — blocking (creates Coordinator internally)
4. `int Op(Descriptor&, ..., time::Interval)` — blocking + timeout

Key operations: `Accept`, `Recv`, `Send`, `Read`, `ReadFile`, `Open`, `Close`, `Shutdown`,
`Connect`

`Accept` args default to `nullptr, nullptr, 0` (addr, addrLen, flags) so existing `Accept(desc)`
call sites continue to work. Pass a `sockaddr*` and `socklen_t*` to retrieve peer addresses.

Blocking variant pattern:
```cpp
int Recv(Descriptor& desc, void* buf, size_t size) {
    Coordinator coord;
    Handle handle(Self(), desc, &coord);
    if (!Recv(handle, buf, size)) return -EAGAIN;
    return handle.Wait();  // blocks via CoordinateWith, returns result or -ECANCELED
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
