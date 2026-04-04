#pragma once

#include <sys/socket.h>

#include "coop/context.h"
#include "coop/signal.h"

namespace coop
{
namespace io
{

struct Descriptor;

// Amortized wake strategy for socket loops that want plain blocking IO in the body of the loop.
// Spawns a detached watcher that waits for the owning context's kill signal and then issues a
// socket shutdown on the descriptor. The guard destructor waits for the watcher to finish before
// descriptor teardown proceeds, so the Descriptor object remains valid while the watcher runs.
//
// This is intentionally a socket-level wake mechanism, not a generic kill-aware replacement for
// every individual IO operation. Blocking *Kill wrappers remain the generic per-call primitive.
//
struct ShutdownOnKillGuard
{
    ShutdownOnKillGuard(Context* owner, Descriptor& desc, int how = SHUT_RDWR);

    ShutdownOnKillGuard(const ShutdownOnKillGuard&) = delete;
    ShutdownOnKillGuard(ShutdownOnKillGuard&&) = delete;

    ~ShutdownOnKillGuard();

  private:
    Context*            m_owner;
    Descriptor*         m_desc;
    Signal*             m_ownerSignal;
    Signal              m_stop;
    Context::Handle     m_watcher;
    int                 m_how;
    bool                m_done;
};

} // end namespace coop::io
} // end namespace coop
