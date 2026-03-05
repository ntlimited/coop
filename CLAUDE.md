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
Size-class allocator for context stack segments, owned by `Cooperator`. 6 power-of-2 buckets
(4KB-128KB), eliminates mmap/malloc syscalls on the spawn/exit hot path. Stack sizes in
`SpawnConfiguration` are transparently rounded up. See `coop/CLAUDE.md` for internals.

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

See `coop/CLAUDE.md` for fast path details (single-arg, two-pass TryAcquire, CoordinateWithKill
specialization).

### CoordinationResult (`coop/coordination_result.h`)
Return type of `CoordinateWith` / `CoordinateWithKill`. Wraps the acquired `Coordinator*` with
sentinel detection:
- `Killed()` — only possible from `CoordinateWithKill`
- `TimedOut()` / `Error()` — check sentinel conditions
- `operator Coordinator*()` / `operator->()` — access the acquired coordinator
- `operator==(Coordinator*)` / `operator==(const Coordinator&)` — pointer comparison

No `operator size_t()`. The `index` field is internal to `MultiCoordinator` / `CoordinateWith`.

### Scheduler Internals (`coop/cooperator.cpp`)
Contexts have three states: `YIELDED` (runnable), `RUNNING` (active), `BLOCKED` (waiting on a
coordinator). The cooperator loop pops yielded contexts, resumes them, and polls io_uring after
each resume. Shutdown spawns a kill context that fires all kill signals. Handlers must check
`IsKilled()` or use `CoordinateWithKill` explicitly for kill-aware IO. See `coop/CLAUDE.md`
for the full loop, shutdown sequence, and loop condition details.

### Context Lifecycle (`coop/context.cpp`)
Contexts form a parent-child tree. Construction registers in parent's `m_children` list.
Kill propagation uses iterative post-order traversal. Destruction: detach, kill children, wait
for children to exit via `m_lastChild` coordinator, then `ContextSwitch(EXITED)` back to the
cooperator. See `coop/CLAUDE.md` for the full lifecycle and Launchable destruction details.

### IO (`coop/io/`)
All IO goes through io_uring. Each operation has 4 variants:
1. `bool Op(Handle&, ...)` — async
2. `bool Op(Handle&, ..., time::Interval)` — async + timeout
3. `int Op(Descriptor&, ...)` — blocking (creates Coordinator internally)
4. `int Op(Descriptor&, ..., time::Interval)` — blocking + timeout

Key operations: `Accept`, `Recv`, `Send`, `Read`, `ReadFile`, `Open`, `Close`, `Shutdown`,
`Connect`, `Sendfile`, `Splice`

Blocking variants use `COOP_IO_IMPLEMENTATIONS` macros or `COOP_IO_IMPLEMENTATIONS_FASTPATH`
(tries a nonblocking direct syscall before io_uring — used by Recv and Send). See
`coop/io/CLAUDE.md` for Handle lifecycle, uring configuration (COOP_TASKRUN, SQPOLL),
fast-path details, and zero-copy operation internals.

**Connect** accepts either a dotted-quad IP or a hostname. Hostnames are resolved cooperatively
via `Resolve4` (DNS over UDP through io_uring). `PlaintextStream` wraps a `Descriptor` for
socket IO (`Recv`, `Send`, `SendAll`). `ReadFile(path, buf, bufSize)` reads an entire file.

### SSL/TLS (`coop/io/ssl/`)
Two BIO modes: **Memory BIO** (default, staging buffer) and **Socket BIO** (real fd, enables
kTLS). See `coop/io/ssl/CLAUDE.md` for BIO modes, kTLS activation, TCP_NODELAY rationale, and
data path dispatch.

### HTTP Server (`coop/http/`)
Route table maps paths to handler functions that receive a `Connection&`:
```cpp
struct Route {
    const char* path;
    void (*handler)(Connection&);
};
```

`Connection` (`coop/http/connection.h`) is the request parser and response writer. Constructed
stack-local per client, owns a 2KB sliding-window recv buffer. Parsing is strictly sequential
(request line -> args -> headers -> body), lazy, and memoized. Each phase implicitly skips the
previous phase if not consumed. All string_views and data pointers are zero-copy into the recv
buffer.

**Bounded vs unbounded**: HTTP elements split into bounded (method, path, arg names, header
names — safe to access directly via string_view/const char*) and unbounded (arg values, header
values, body — delivered via `Chunk*` with zero-copy and completion flag).

**Flyweight pattern**: `GetRequestLine()` returns `RequestLine*` (null = failure).
`ReadArgValue()`, `ReadHeaderValue()`, `ReadBody()` return `Chunk*` (null = end/error).
Flyweight instances are Connection members, no heap allocation.

**Pull API**:
```cpp
Connection conn(desc, ctx, co, timeout);
auto* req = conn.GetRequestLine();      // RequestLine* (method, path)
while (auto* name = conn.NextArgName()) // query string args
    auto* val = conn.ReadArgValue();    // Chunk* (zero-copy)
while (auto* name = conn.NextHeaderName())
    auto* val = conn.ReadHeaderValue();
while (auto* chunk = conn.ReadBody())   // handles chunked TE internally
    process(chunk->data, chunk->size);
conn.Send(200, "text/plain", body);
```

**Response methods**: `Send` (status + body), `SendHeaders` (headers only), `BeginChunked` /
`SendChunk` / `EndChunked` (chunked response), `Sendfile` (zero-copy file serving).

`RunServer` accepts connections in a loop, launches an `HttpConnection` (Launchable, 32KB stack)
per client. No method filtering in framework — handlers decide.

**Keep-alive**: HTTP/1.1 keep-alive is enabled by default. `HttpConnection::Launch` loops over
requests on the same connection. `Connection::Reset()` reinitializes parser state between
requests, preserving leftover buffer data for pipelining. The loop exits on send error, kill,
or when the client sends `Connection: close`. The `Connection` header detection piggybacks on
the existing special-header mechanism (alongside Content-Length / Transfer-Encoding).
`KeepAlive()` returns the current keep-alive state. `SkipBody()` drains unconsumed body bytes
before `Reset()` to keep the parser positioned correctly.
Optional `searchPaths` for static file fallback, optional `timeout` (default 30s).

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

See `DESIGN_IDIOMS.md` for patterns and idioms that should guide new API design.

## Building

The project requires liburing >= 2.3 (Ubuntu 24.04+). A Docker-based build environment is
provided for hosts with older liburing:

- `./docker-build.sh` — debug build (or `./docker-build.sh release`)
- `./docker-test.sh` — run tests (or `./docker-test.sh debug --gtest_filter=IO.*`)

The host kernel's io_uring feature level doesn't matter for compilation — `uring.cpp` has compat
defines. Runtime fallbacks in `Uring::Init()` handle missing kernel features gracefully.

# Debugging guidelines

- Tests should be run in debug mode; release tests are not unimportant but should never be run
  before debug tests pass.
- Targetted usage of the gtest filter for changes is encouraged to save time, but run all tests
  when called for - making foundational changes

## Debugging Strategy

- Running tests with GDB attached can save time, both for diagnosing potentially hanging tests
  and investigating crashes that would be lost/harder to investigate after the process is dead
