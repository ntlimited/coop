#include "reactor.h"

#include <poll.h>

#include <chrono>
#include <memory>
#include <unordered_map>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/io/descriptor.h"
#include "coop/io/poll.h"
#include "coop/time/sleep.h"

namespace coop
{
namespace io
{

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

    void SpawnWatcher(std::unique_ptr<Watch> owned)
    {
        Watch* w = owned.get();
        watches[w->fd] = w;

        co->Spawn([this, held = std::move(owned)](Context* c) mutable
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

            while (!w->removed)
            {
                unsigned mask = w->mask;
                int rev = Poll(desc, mask);
                if (w->removed) break;

                unsigned revents = (rev < 0) ? static_cast<unsigned>(POLLERR)
                                             : static_cast<unsigned>(rev);
                onReady(w->fd, revents, user);
                // Loop re-checks w->removed: onReady may have Unwatched this fd (e.g. its transfer
                // finished) inside the call above.
            }

            // Retire — but only if the map still points at us. A reused fd may have installed a
            // fresh watcher over our slot; leave that one alone. `held` then frees our Watch.
            //
            auto it = watches.find(w->fd);
            if (it != watches.end() && it->second == w)
            {
                watches.erase(it);
            }
        });
    }
};

Reactor::Reactor(Cooperator* co, ReadyFn onReady, TimeoutFn onTimeout, void* user)
    : m_impl(new Impl{co, onReady, onTimeout, user, {}, 0})
{
}

Reactor::~Reactor()
{
    delete m_impl;
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

    m_impl->SpawnWatcher(std::make_unique<Impl::Watch>(Impl::Watch{fd, mask, false}));
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

    Impl* impl = m_impl;
    impl->co->Spawn([impl, gen, ms](Context* c)
    {
        c->SetName("reactor-timer");
        c->Detach();

        // Same off-stack deferral as the watcher (see SpawnWatcher).
        //
        if (ms > 0) time::Sleep(c, std::chrono::milliseconds(ms));
        else        c->Yield(true);

        if (gen != impl->timerGen) return;   // a newer SetTimeout replaced us
        impl->onTimeout(impl->user);
    });
}

} // end namespace coop::io
} // end namespace coop
