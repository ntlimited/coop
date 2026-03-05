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
