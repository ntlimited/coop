#include "reactor.h"

#include <poll.h>

#include <chrono>
#include <memory>
#include <unordered_map>
#include <utility>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/io/descriptor.h"
#include "coop/io/poll.h"
#include "coop/time/sleep.h"

namespace coop
{
namespace io
{

// Lifetime covenant
// -----------------
// What keeps Impl alive for exactly as long as any context can still dereference it:
//
//   Impl is owned by a std::shared_ptr, and EVERY context this Reactor spawns captures its own
//   strong reference into the lambda that lives on that context's stack. A context therefore
//   cannot outlive the state it reads: the reference is released by the lambda's destructor,
//   which the runtime runs on the context's own stack after the body returns. ~Reactor drops
//   only the Reactor's reference. The last of {the Reactor, every parked context} to go away is
//   what frees Impl, and that is decided by the refcount rather than by anyone's ordering
//   assumption.
//
// That alone would be memory-safe and still wrong. Impl outliving the Reactor does not keep the
// *caller's* objects alive: onReady/onTimeout and `user` point into whatever built the bridge,
// and that dies with (or before) the Reactor. So keeping the state alive is only half the
// contract — the other half is that ~Reactor sets `dead`, and every context re-reads it after
// every suspension point before touching a callback or `user`. Alive-enough-to-read, never
// alive-enough-to-call-back.
//
// The flag is deliberately a field of the refcounted Impl and not of the Reactor. A liveness token
// stored in the object whose death it reports is not a liveness token; reading it would be the
// very use-after-free it claims to prevent.
//
// Destruction is not a synchronisation point. ~Reactor does not cancel or join the contexts it
// spawned, because joining them would mean blocking the destroying context until a parked timer's
// deadline elapsed or a watched fd happened to become readable — turning a destructor into an
// unbounded wait, and deadlocking outright if the Reactor is destroyed from inside one of its own
// callbacks (the context would be waiting for itself). Parked contexts instead retire themselves
// the next time they wake, which for a timer is at its deadline and for a watcher is on readiness
// or on the cooperator's shutdown kill. Until then they hold a small Impl alive, which is the
// price of a destructor that can never block.
//
struct Reactor::Impl
{
    // Per-fd watch state. Owned by its watcher context (not the map), which frees it on exit; the
    // map holds a non-owning pointer. This is what lets Watch replace a still-draining watcher for a
    // reused fd without freeing state the old watcher is still reading.
    //
    struct Watch
    {
        int      fd;
        unsigned mask;
        bool     removed = false;
    };

    Cooperator*     co;
    Reactor::ReadyFn   onReady;
    Reactor::TimeoutFn onTimeout;
    void*           user;

    std::unordered_map<int, Watch*> watches;   // non-owning; the watcher context owns the Watch
    long            timerGen = 0;

    // Set by ~Reactor. Guards the callbacks and `user`, which belong to the Reactor's owner and do
    // not survive it — unlike Impl, which does. Read only on the cooperator's thread.
    //
    bool            dead = false;

    // Static, and takes its own strong reference explicitly: a member function would capture a raw
    // `this` into the spawned lambda, which is exactly the bug this file exists to not have.
    //
    static void SpawnWatcher(std::shared_ptr<Impl> const& self, std::unique_ptr<Watch> owned)
    {
        Watch* w = owned.get();
        self->watches[w->fd] = w;

        self->co->Spawn([impl = self, held = std::move(owned)](Context* c) mutable
        {
            c->SetName("reactor-fd");
            c->Detach();

            // Defer off the spawning call stack. Eager Spawn runs us synchronously inside whatever
            // call registered this fd — often a foreign callback firing inside io::Poll's inline
            // completion for an already-ready fd. Yielding once guarantees our onReady callbacks
            // originate from the scheduler loop, never nested in a foreign (non-reentrant) call.
            //
            c->Yield(true);

            Watch* w = held.get();
            Descriptor desc(io::borrowed, w->fd);   // foreign code owns the fd — never close it

            // `impl->dead` is re-read on every pass, next to `w->removed`: the Reactor can be
            // destroyed while we are parked in Poll below, and firing onReady afterwards would
            // call through a function pointer and a `user` that its owner has already destroyed.
            //
            while (!w->removed && !impl->dead)
            {
                unsigned mask = w->mask;
                int rev = Poll(desc, mask);
                if (w->removed || impl->dead) break;

                unsigned revents = (rev < 0) ? static_cast<unsigned>(POLLERR)
                                             : static_cast<unsigned>(rev);
                impl->onReady(w->fd, revents, impl->user);
                // Loop re-checks w->removed: onReady may have Unwatched this fd (e.g. its transfer
                // finished) inside the call above.
            }

            // Retire — but only if the map still points at us. A reused fd may have installed a
            // fresh watcher over our slot; leave that one alone. `held` then frees our Watch.
            //
            // Safe to touch `watches` even when dead: the map is a member of Impl, which our own
            // strong reference is still keeping alive. Only the callbacks and `user` are gone.
            //
            auto it = impl->watches.find(w->fd);
            if (it != impl->watches.end() && it->second == w)
            {
                impl->watches.erase(it);
            }
        });
    }
};

Reactor::Reactor(Cooperator* co, ReadyFn onReady, TimeoutFn onTimeout, void* user)
    : m_impl(std::make_shared<Impl>(Impl{co, onReady, onTimeout, user, {}, 0, false}))
{
}

Reactor::~Reactor()
{
    // Cancellation, not a join. Everything still parked keeps Impl alive through its own reference
    // and retires on its next wake; this flag is what stops any of them calling back into an owner
    // that is going away. Dropping m_impl below releases only the Reactor's reference.
    //
    m_impl->dead = true;
}

void Reactor::Watch(int fd, unsigned mask)
{
    if (mask == 0)
    {
        Unwatch(fd);
        return;
    }

    auto it = m_impl->watches.find(fd);
    if (it != m_impl->watches.end() && !it->second->removed)
    {
        it->second->mask = mask;   // live watcher re-reads mask on its next loop
        return;
    }

    Impl::SpawnWatcher(m_impl, std::make_unique<Impl::Watch>(Impl::Watch{fd, mask, false}));
}

void Reactor::Unwatch(int fd)
{
    auto it = m_impl->watches.find(fd);
    if (it != m_impl->watches.end())
    {
        it->second->removed = true;   // watcher sees this after its poll and self-retires
    }
}

void Reactor::SetTimeout(int64_t ms)
{
    long gen = ++m_impl->timerGen;   // supersede any pending timeout
    if (ms < 0) return;

    m_impl->co->Spawn([impl = m_impl, gen, ms](Context* c)
    {
        c->SetName("reactor-timer");
        c->Detach();

        // Same off-stack deferral as the watcher (see SpawnWatcher).
        //
        if (ms > 0) time::Sleep(c, std::chrono::milliseconds(ms));
        else        c->Yield(true);

        // Order matters: `dead` first. A superseded timer and a timer whose Reactor has been
        // destroyed both retire silently, but only the second would be reading a destroyed owner's
        // callback if it got as far as acting on the timerGen comparison.
        //
        if (impl->dead) return;              // the Reactor, and the user data, are gone
        if (gen != impl->timerGen) return;   // a newer SetTimeout replaced us
        impl->onTimeout(impl->user);
    });
}

} // end namespace coop::io
} // end namespace coop
