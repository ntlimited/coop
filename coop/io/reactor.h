#pragma once

#include <cstdint>

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
    Impl* m_impl;
};

} // end namespace coop::io
} // end namespace coop
