#pragma once

#include "coordinator.h"

namespace coop
{

struct Context;

// Signal is a one-shot broadcast notification primitive. It starts in the "armed" state, held by
// an owning context. Other contexts can Wait on it, blocking until Notify is called. Once notified,
// all waiting contexts are unblocked and future Wait calls return immediately.
//
// This is the mechanism behind context kill signals. For use with MultiCoordinator / CoordinateWith
// patterns, AsCoordinator() provides a compatible Coordinator pointer.
//
struct Signal
{
    Signal(Signal const&) = delete;
    Signal(Signal&&) = delete;

    // Create a signal armed by the given owning context.
    //
    Signal(Context* owner);

    bool IsSignaled() const;

    // Block the calling context until Notify is called. Returns immediately if already signaled.
    //
    void Wait(Context* ctx);

    // Signal all waiting contexts. Once called, future Wait/TryAcquire calls return immediately.
    //
    void Notify(Context* ctx, bool schedule = true);

    // For MultiCoordinator / CoordinateWith compatibility.
    //
    Coordinator* AsCoordinator() { return &m_coord; }

  private:
    bool m_signaled;
    Coordinator m_coord;
};

} // end namespace coop
