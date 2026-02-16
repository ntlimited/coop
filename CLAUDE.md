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
    return handle;  // blocks, returns result via operator int()
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
