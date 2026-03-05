# Design Idioms

Patterns and idioms that guide API design in this project. These are not rules to check
mechanically — they are orientations that reflect how we think about interfaces and data flow.

## Flyweight null returns over out-params

When a method can fail, return a lightweight value type with `operator bool`. The failure state is
backed by a nullptr or sentinel — no heap allocation, no separate error code. The caller checks
truthiness and dereferences on success.

```cpp
auto path = conn.GetPath();
if (!path) return;
if (*path == "/api/status") { ... }
```

This avoids the verbosity of bool + out-param, the forgettability of sticky error state, and the
ceremony of a full Result type. The flyweight itself should be trivially constructible and should
convert implicitly to the underlying type for ergonomic use.

## No vtables or function pointers on hot paths

Virtual dispatch is acceptable at per-connection or per-request granularity (e.g.
`Launchable::Launch()`). It is not acceptable for per-element operations — per header, per byte,
per parsed token. When the alternative to a callback interface is a pull model or template
dispatch, prefer those.

The cost is not just the indirect call. It is the lost inlining, the branch predictor pollution,
and the fact that a vtable is a contract that is hard to change later.

## Contiguous layout over pointer indirection

Objects on the hot path should be contiguous allocations. Variable-size data (buffers, arrays)
belongs as a trailing flexible member (`char m_buf[0]`), not as a separately-allocated pointer.
A pointer dereference is a memory load; a trailing member is a compile-time offset from `this`.

This means ABC (abstract base class) inheritance is a poor fit for data-owning hot-path objects,
because the base class cannot access trailing data in the derived class without going through a
pointer or virtual call. If a base class needs the buffer on every parse operation, the hierarchy
has turned a compile-time offset into a runtime load — exactly the wrong trade.

## Pure-virtual interfaces are API sugar, not data owners

Pure-virtual interfaces (`struct Base { virtual void Method() = 0; }`) are appropriate at API
boundaries — handler signatures, plugin interfaces, test seams. Virtual dispatch at those
boundaries costs one indirect call per handler invocation, which is noise.

Pure-virtual interfaces are *not* appropriate as the implementation base for objects that own data
and access it on every operation. When the same object needs both a clean API surface and
zero-overhead internal access, use CRTP for the implementation and a pure-virtual interface for
the API boundary. These are separate concerns with separate solutions.

## CRTP for implementation polymorphism

When a base class needs zero-overhead access to derived-type members (buffer, transport, config),
use CRTP: `template<typename Derived> struct Impl : Interface`. The base accesses derived data via
`static_cast<Derived*>(this)->m_member`, which the compiler resolves to a direct offset — no
pointer load, no virtual call.

The general pattern when both a stable API and zero-overhead internals are needed:

1. **Pure-virtual interface** — what callers receive (`ConnectionBase&`). Defines the API.
2. **CRTP implementation** — all logic lives here (`ConnectionImpl<Derived>`). Accesses derived
   members via `static_cast`. Explicit template instantiation keeps the code in a `.cpp` file.
3. **Final concrete type** — owns the data (`Connection<Transport>`). Provides CRTP access points
   and the trailing flexible member.

Virtual dispatch happens once at the API boundary. Internally, everything is compile-time resolved.
The `.cpp` file stays manageable via explicit instantiation for the known concrete types.

## Pull over push when the consumer drives pacing

When the caller naturally controls flow — parsing a protocol, reading structured data, iterating
results — a pull model (`Next()`, `Get()`) is preferred over push (framework calls callbacks).

Pull keeps control flow linear and readable, avoids vtables, and lets the caller skip work it does
not care about. Push is appropriate when the producer has information the consumer cannot predict
(event arrival, signal delivery), not when the consumer is walking a known structure.

## Chunk delivery over buffer-fits-all

Do not force data to fit in a single buffer. Deliver what is available with a completion flag. Let
the consumer decide whether to accumulate or process incrementally.

The zero-copy common case (data fits in the current buffer) should be free. The boundary-spanning
case should cost a small copy at worst, never a design constraint imposed on callers. Fixed-size
limits (`MAX_HEADERS`, `MAX_LINE_LENGTH`) are a smell — they push the framework's implementation
constraints onto the user.

## Early decision support

APIs should let callers act on partial information. If a handler can reject a request after seeing
the method, the framework should not force it to wait for headers and body. If a validator can fail
on the first byte, it should not need the entire input buffered.

This means structuring APIs so that each piece of information is independently accessible as soon as
it is available, and responses or errors can be issued at any point — not only after all input is
consumed.

## Do not do work nobody asked for

Framework code should not parse, allocate, or store data the caller has not requested. If nobody
calls `NextHeader()`, headers should not be parsed. If nobody captures a header value, it should
not be buffered. If the caller skips the body, the body should not be read from the network.

This is not lazy evaluation for its own sake. It is the principle that in a cooperative system, the
cost of an operation should be borne by the context that benefits from it, and the framework should
not preemptively consume resources (CPU, memory, IO bandwidth) on speculation.

## IKWIAD (in release mode)

"I Know What I am Doing" is a core principal for us. We're a micro-stack, green threading library:
"Danger" is our user's middle name. We can offer safety checks but they should:

- Offer zero-cost disabling mechanics
- Disable by default in release builds (but we may allow developers to leave them enabled)

Start with clean idioms and strong documentation, then give the user guns to point in any direction
they want (feet included).

Corollary: **diagnose misuse, don't work around it.** When a user does something wrong, the right
response is a compile-time error (`static_assert`) or a debug-mode assert — not silent machinery
that "handles" the wrong thing at runtime cost. If an interface type needs a virtual destructor for
a pattern to work, assert that it has one; don't build a parallel type-erased destruction path to
cover for the case where it doesn't. Workaround machinery adds bytes, complexity, and code that
exists only to defend against a mistake that a one-line diagnostic would catch instantly. The user
who sees `static_assert: Interface type must have a virtual destructor` fixes it in seconds. The
machinery you built to avoid that message costs everyone forever.
