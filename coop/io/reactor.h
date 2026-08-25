#pragma once

#include <cstdint>
#include <memory>

namespace coop
{

struct Cooperator;

namespace io
{

// Reactor — a readiness/timer event-loop surface backed by io_uring, for bridging foreign libraries
// that expect an epoll-style reactor: libcurl's multi interface, c-ares, libpq's async API, or any
// code written against "tell me when this fd is readable/writable, and call me after this timeout."
//
// You register fds and a single timeout; the reactor calls back when they fire — from a coop context
// and, crucially, never nested inside one of your callbacks. It owns the two things that make such a
// bridge subtle on a completion-based, cooperative substrate:
//
//   * one POLL_ADD watcher context per fd, transient and self-terminating (Unwatch, or the fd going
//     away, retires it — no handles to track);
//   * a yield off the spawning call stack, so a callback that re-enters a non-reentrant foreign API
//     (curl_multi_socket_action calling back into SOCKETFUNCTION, say) can never drive the reactor
//     recursively.
//
// Opt-in and zero-cost when unused: a plain object with no global state, instantiated only where a
// bridge is built. See examples/curl_coop.h for a complete libcurl driver in ~60 lines on top of it.
//
// Threading: single-cooperator, like everything else — construct and use a Reactor from one
// cooperator's context(s). Callbacks run on that cooperator's thread.
//
// Lifetime note: Unwatch(fd) means the fd is going away; re-Watching the same fd number before its
// previous watcher has drained is not supported (the foreign libraries this targets never do it —
// a socket is removed when its transfer/connection ends, not mid-flight).
//
// Destruction is non-blocking and safe with work still parked. A Reactor may be destroyed while a
// timeout is pending or watchers are blocked on their polls: destroying it is a cancellation, not a
// join. It never waits for a parked context, so it can be destroyed from anywhere a destructor can
// run — including from inside a ReadyFn/TimeoutFn callback, where waiting for the reactor's own
// contexts would be waiting on the very callback that is running.
//
// The rule that makes that safe: the shared state the spawned contexts read is owned by a
// shared_ptr that each of them holds, so it outlives the Reactor by construction, and ~Reactor
// marks it dead so no callback can fire afterwards. See the invariant in reactor.cpp.
//
class Reactor
{
public:
    // Callback: fd is ready with poll(2) revents (POLLIN/POLLOUT/POLLERR/POLLHUP). `user` is the
    // pointer handed to the constructor.
    //
    using ReadyFn   = void (*)(int fd, unsigned revents, void* user);
    using TimeoutFn = void (*)(void* user);

    Reactor(Cooperator* co, ReadyFn onReady, TimeoutFn onTimeout, void* user);
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    // Watch fd for the given poll(2) event mask (POLLIN, POLLOUT, or both). Re-call to change the
    // mask; mask 0 is the same as Unwatch. Safe to call from within a ReadyFn/TimeoutFn callback.
    //
    void Watch(int fd, unsigned mask);
    void Unwatch(int fd);

    // Arm the single timeout: < 0 cancels, 0 fires as soon as possible, > 0 fires after ms
    // milliseconds. A new call supersedes any pending timeout. Safe to call from within a callback.
    //
    void SetTimeout(int64_t ms);

private:
    struct Impl;

    // Shared, not owned outright: contexts this Reactor spawned outlive it (see the covenant in
    // reactor.cpp), and each holds a strong reference. ~Reactor drops one reference and sets the
    // dead flag; the last parked context to retire frees the state.
    //
    std::shared_ptr<Impl> m_impl;
};

} // end namespace coop::io
} // end namespace coop
