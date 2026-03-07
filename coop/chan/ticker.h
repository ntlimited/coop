#pragma once

#include "coop/chan/channel.h"
#include "coop/coordinate_with.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"

// Ticker is a periodic Channel<void> that fires approximately every interval. Composes
// naturally with On() and SelectAnyVoid(). Tick delivery is best-effort: if the channel is
// full when the ticker fires (the receiver has not yet consumed the previous tick), the tick
// is dropped rather than queued or delayed.
//
// Usage:
//
//   coop::Ticker ticker(ctx, std::chrono::milliseconds(10));
//
//   while (coop::SelectWithKill(ctx,
//       coop::On(ticker.Chan(), [&]{ onTick(); })
//   )) {}
//
//   ticker.Stop();
//
// The constructor yields to start the ticker context; by the time it returns, the ticker
// is running and waiting on its first interval. Stop() (and ~Ticker()) block until the
// ticker context releases all references to Ticker members, so it is safe to destroy the
// Ticker immediately after Stop() returns.
//
// Stop() must be called from a cooperating context. It is idempotent.
//

namespace coop
{
namespace chan
{

struct Ticker
{
    Ticker(Context* ctx, time::Interval interval);
    ~Ticker();

    // Stop the ticker and wait for it to exit. Idempotent: safe to call multiple times.
    //
    void Stop();

    // The channel that receives a tick on each interval fire.
    //
    Channel<void>& Chan() { return m_ch; }

  private:
    FixedChannel<void, 1> m_ch;

    // m_stop starts held (by the constructor's ctx). Stop() releases it to wake the ticker.
    // No Context::Handle is stored — avoids ~Context() touching Ticker members after exit.
    //
    Coordinator m_stop;

    // Held by the ticker while it runs. Flash() in Stop() waits for the ticker to release
    // it, ensuring no Ticker member is accessed after Stop() returns.
    //
    Coordinator m_exit;
};

inline Ticker::Ticker(Context* ctx, time::Interval interval)
: m_ch(ctx)
, m_stop(ctx)
{
    // Spawn copies the lambda to the ticker context's stack and immediately switches to it
    // (EnterContext yields the spawner). By the time the constructor returns, the ticker
    // has acquired m_exit and blocked in CoordinateWithKill on its first interval.
    //
    Spawn([this, interval](Context* tickCtx)
    {
        // Hold m_exit while running. Stop()/~Ticker() Flash() on m_exit to ensure no
        // Ticker member is accessed after Stop() returns.
        //
        m_exit.Acquire(tickCtx);

        while (true)
        {
            // Wait for: kill signal | explicit stop | interval timeout.
            //
            auto r = CoordinateWithKill(tickCtx, &m_stop, interval);
            if (r.Killed() || r == &m_stop) break;

            // Interval fired — send a tick. TrySend drops the tick if the channel is full
            // (receiver is behind), preserving best-effort semantics.
            //
            m_ch.TrySend();
        }

        m_exit.Release(tickCtx);
    });
    // Spawn immediately switches to the ticker. By the time we resume here, the ticker
    // has acquired m_exit and is blocked in CoordinateWithKill. m_exit is held.
    //
}

inline void Ticker::Stop()
{
    if (m_exit.IsHeld())
    {
        // Release m_stop to wake the ticker (if it is waiting on the interval rather than
        // a kill). Then Flash m_exit to block until the ticker releases all references.
        //
        m_stop.Release(Self());
        m_exit.Flash(Self());
    }
    m_ch.Shutdown();
}

inline Ticker::~Ticker()
{
    Stop();
}

} // end namespace chan
} // end namespace coop
