#pragma once

#include <csignal>
#include <cstddef>

namespace coop
{

// SignalStack — an alternate stack for signal handlers on the calling OS thread.
//
// A cooperator runs application code on small pool-allocated segments, and a signal that arrives
// while a context is scheduled runs its handler on that context's stack. That is a bad place for it.
// The kernel's signal frame is sized by the widest register file the CPU has — on an AVX-512 machine
// glibc's own answer for how much room a handler needs is tens of kilobytes — while a context deep
// in a call chain may have a couple of kilobytes left. The frame goes over the end of the segment
// into the guard page below it, and the SIGSEGV that follows is reported against whatever the
// context was doing rather than against the signal. It correlates with load, because load is what
// makes stacks deep, and it reproduces nowhere.
//
// An alternate stack gives the thread somewhere else to put those frames. It is per-thread state,
// installed for as long as this object lives; construct one on every thread that can take a signal.
//
// Installing it is only half of the arrangement. The other half is SA_ONSTACK on the handler, which
// is a property of the registration rather than of the thread — see RegisterOnAltStack. A handler
// registered without that flag still runs on the context stack no matter how many alternate stacks
// are installed.
//
// If the thread already has an alternate stack large enough — one a sanitizer runtime or the host
// process installed — this adopts it rather than replacing it, and IsInstalled reports false.
//
struct SignalStack
{
    SignalStack();
    ~SignalStack();

    SignalStack(SignalStack const&) = delete;
    SignalStack(SignalStack&&) = delete;

    // Whether this object installed the stack the thread is using. False also when an adequate
    // alternate stack was already present, which is a success for the caller's purposes; use
    // IsActive to ask the question the caller usually means.
    //
    bool IsInstalled() const { return m_installed; }

    // Whether the calling thread has an alternate signal stack at all, from any source.
    //
    static bool IsActive();

    // The installed region, or {nullptr, 0} if this object adopted an existing stack.
    //
    void const* Bottom() const;
    size_t      Size() const { return m_size; }

    // How much to reserve.
    //
    // Resolved at run time and never as a constant. glibc 2.34 redefined SIGSTKSZ and MINSIGSTKSZ as
    // calls to sysconf(_SC_SIGSTKSZ), so that a binary built on one machine gets the right answer on
    // another with a different register file. The consequence for callers is that neither can size
    // an array, seed a constexpr, or be assumed cheap.
    //
    static size_t PreferredSize();

  private:
    void*   m_memory;
    size_t  m_size;
    stack_t m_previous;
    bool    m_installed;
};

// Register a signal handler that will run on the alternate stack of whichever thread takes the
// signal. This is sigaction with SA_ONSTACK forced on, and it exists so the flag is not the thing a
// caller forgets: omitting it silently puts the handler back on the context stack and leaves the
// alternate stack sitting unused.
//
// Delivery is the caller's remaining problem. A process-directed signal — SIGINT from a terminal,
// SIGPROF from ITIMER_PROF — goes to an arbitrary thread that does not block it, so every thread
// that might be chosen needs its own SignalStack. Cooperator threads have one for the life of their
// scheduler loop; threads the host process owns do not, unless the host constructs one.
//
// Returns 0 on success, or -1 with errno set, matching sigaction.
//
int RegisterOnAltStack(
    int signo,
    void (*handler)(int, siginfo_t*, void*),
    int flags = 0,
    struct sigaction* previous = nullptr);

} // end namespace coop
