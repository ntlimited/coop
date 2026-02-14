# coop: cooperative multitasking framework

## Background

There are two general manners in which cooperator multitasking can occur with 'synchronous' coding
patterns. Transformative, non-stack based approaches which effectively map code blocks into structs,
state machines, and callbacks (usually as a native language feature such as with C++20's coroutine
system) or stack-based approaches that effectively run as userspace schedulers by manipulating
"thread" state manually.

This library is the latter, both because the former already exists in C++, and because the latter
frankly lends itself to many interesting coding patterns due to the stack-based allocation patterns
that are inherently fun to build.

## Coding Patterns

The most significant pieces of the framework are `coop::Coordinator` and the `Coop::EmbeddedList`
template. These allow building extremely efficient data structures to manage state across tasks,
without dynamic allocation (modulo of course the stacks themselves). The `coop::Coordinator` _is_
effectively the system as far as a lot of code goes, alongside its `coop::Coordinated` sibling.
These are the hooks into the blocking/unblocking of tasks within the scheduler.

### Handles

`Coordinator` instances are a very strong building block, but for higher level interfaces `Handle`
types are the preferred idiom. These wrap a `Coordinator` and essentially are used to build the
higher level semantics for whatever task is at hand - see `time::Handle` and `io::Handle`

### Blocking vs non-blocking APIs.

While the point of cooperative multitasking is that we get to have nice blocking APIs that are
both cheap and friendly to use, practically speaking we still want the ability to line up
several things and then wait afterwards. It is encouraged to provide a low-level "setup" API
that takes a `Coordinator` or package-appropriate `Handle` type and then offer a wrapped,
blocking version for those who don't care. `coop/io/connect.h` is a decent example of this.

### Task trees and killing tasks

Tasks are spawned with an inherent parent/child relationship so that we have some facscimile of a
workable guarantee such that when a task is spawned it can trust that things it relied on existing
at spawn time continue to exist.

Tasks also can be killed, which is recursive - killing a task kills all its children. It is
considered wise to make APIs kill-check-aware e.g. using `CoordinateWithKill` and other such APIs.
The stuff that makes this work properly is fairly nontrivial but it's pretty well debugged in its
current form at least.

### Thread locals

For obvious reasons, thread locals are shunned _other than_ the two requisite thread local of the
`Cooperator` itself (the userspace scheduler). There is unfortunately not a strongly opinionated
enough way of writing APIs that operate on "the current task" and probably we should just defer to
always pulling it off the thread local (e.g. `coop::Self()`) but that's a problem for another day.

## Awful shitty TODOs

### Compiler friendliness and stack hacks

There's a few ways that the compiler can make itself too helpful and generate code that our inline
assembly stack-jumping can invalidate. Probably there are better ways to tell gcc/llvm to be aware
and avoid this but it's not my personal forte.

Setting up stack guard pages is not that hard but I also didn't do that yet.

### Stack traces

Making stack traces work here is going to be un-fun and not something I was scared enough to try
and chew off.
