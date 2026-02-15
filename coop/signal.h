#pragma once

#include "coordinator.h"
#include "time/interval.h"

namespace coop
{

struct Context;
namespace detail
{
    struct AmbiResult;

    template<typename... Coords>
    AmbiResult CoordinateWithImpl(Context*, Coords...);

    template<typename... Coords>
    AmbiResult CoordinateWithTimeoutImpl(Context*, time::Interval, Coords...);
}

// Signal is a one-shot broadcast notification primitive. It starts in the "armed" state, held by
// an owning context. Other contexts can Wait on it, blocking until Notify is called. Once notified,
// all waiting contexts are unblocked and future Wait calls return immediately.
//
// This is the mechanism behind context kill signals. CoordinateWith is the sole consumer of
// the internal coordinator — it handles cleanup to prevent re-arming after MultiCoordinator's
// TryAcquire sets m_heldBy.
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

    // Signal all waiting contexts. Once called, future Wait calls return immediately.
    //
    void Notify(Context* ctx, bool schedule = true);

  private:
    template<typename... Coords>
    friend detail::AmbiResult detail::CoordinateWithImpl(Context*, Coords...);

    template<typename... Coords>
    friend detail::AmbiResult detail::CoordinateWithTimeoutImpl(
        Context*, time::Interval, Coords...);

    Coordinator* AsCoordinator() { return &m_coord; }

    // Reset internal coordinator after MultiCoordinator consumes it via TryAcquire. Without this,
    // TryAcquire re-arms m_heldBy and future uses would deadlock.
    //
    void ResetCoordinator() { m_coord.m_heldBy = nullptr; }

    bool m_signaled;
    Coordinator m_coord;
};

} // end namespace coop
