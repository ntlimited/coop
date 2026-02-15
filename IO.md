# IO package thoughts

The IO package is the most critical piece of the package outside of the raw building blocks
(Context, Coordinator) because anything other than the most toy app will need to use it. We should
invest very heavily in usability and performance.

## Current patterns

### Boilerplate multiplication

We have a set of patterns that I absolutely love: a pulled-from-libc stand-in for the syscall
wrapper (`int Recv(Descriptor, void*, size_t, int)`) that offers the blocking interface developers
know and tolerate; a composable "async" flavor with `bool Recv(Handle, void*, size_t, int)`. This is
great for developers who might use this library; it is not particularly fun for those who develop it
however as this is deeply unimaginative boilerplate to repeat. We of course can use AI techniques to
bulk generate it, but even this is of questionable in my view: it still has costs in terms of the
extra tokens used in analysis that ideally we'd like to cut down on. My usual toolkit here is
templates, macros, and templates with macros. _Assuming_ `io_uring` took a sane approach (and if
they didn't, I'm probably more likely wrong about the sanity) then this seems that macros could
easily work, e.g.:

```
#define IO_OPERATION(FN, OP, ...) \
bool FN(Handle& handle, ARGS) \
{ \
    auto* sqe = io_uring_get_sqe(&desc.m_ring->m_ring); \
    if (!sqe) \
    { \
        return -EAGAIN; \
    } \
    io_uring_prep ## OP(sqe, handle.m_descriptor->m_fd __VA_OPT__(,) __VA_ARGS__); \
    handle.Submit(sqe); \
    return true;
} \
...
```

Extending this to the 'blocking' flavor is straightforward to imagine. Templates are not impossible
per se but the necessity to use functions likely brings us back into needing macros for any kind of
friendly approach.

Is this possible? I think the `io_uring_prep_...` naming/arg conventions are the principal risk.

### Timeouts

We added timeout support to `Recv` as a proof of concept, but the approach as it is remains somewhat
half baked - literally, as we only support the "blocking" signature. There's a reason for this; we
need stack space to declare the actual timespec. I would like to extend this to all APIs - the
beauty of `io_uring` is that this genericizes trivially - but this requires us to change the "use
the function's stack" approach as well as imputes the boilerplate multiplication issue. The former
issue is most easily solved by adding storage for the `__kernel_timespec` to every `Handle`;
wasteful perhaps but how many of these will a given stack or object contain? Alternatively we could
create a new wrapper - `HandleWithTimeout`? Ugly naming for sure - either by composition or
inheritance. The latter would presumably be solvable via the suggestions in the preceding section.


## Dangling threads

### File descriptor registration

There is a long stanza of TODO thoughs  in the `Uring` class regarding `io_uring`'s support for
registering file descriptors: this is a - to my understanding - meaningful speedup, but also in
my view extremely difficult for a general purpose library to have a detailed enough understanding
of application behaviors to make appropriate tradeoffs. There are four options:

- Never registration
- Always registration
- Opt-out registration
- Opt-in registration

The first mode is largely where we are today, despite unused cruft. This is probably the safest
default; we don't risk tying ourselves in knots (cleanly doing registration semantics under
an arbitrary workload feels painful to avoid some level of goofy overhead for at least _some_
usecases) and many applications won't care. _But_ I personally feel that both specifically and
generally, if this library has value then it needs to be power-user oriented and ideally we
should not leave stones unturned if there's meaningful performance implications.

Always registration, the second mode, is almost certainly infeasible. The overhead of manipulating
this kernel state for arbitrary workloads might well involve ~doubling the number of SQEs associated
with an operation - open, do something, close adds register/unregister and some unpleasant amount of
userspace bookkeeping - plus there can be near arbitrarily many FDs to hold state information for
and we'd get into unpleasant dynamism which is a further potential performance vampire.

The third and forth are basically interchangeable, modulo the implication of having relevant state
bits potentially in `Descriptor` directly versus a second wrapper/related type being more (IMO)
appropriate in the opt-out mode. Overall I think opt-in is best; however we should also at least
try to quantify the actual performance benefit, at least using some kind of goofy synthetic test
with a large number of socket pairs ping-ponging.
