#pragma once

#include "countdown_latch.h"

namespace coop
{

struct Context;

// Answers whether dispatched work spawned, not whether that work has completed.
// The generic countdown carries the cross-thread result and retains the caller's
// wake reader through shutdown. Wait must run before destruction when Cooperate
// accepted a request with this handle. It joins even when the caller is killed.
struct CooperateHandle
{
    CountdownLatch m_completion{1};
    bool m_spawnOk{false};

    explicit CooperateHandle(Context*) {}

    bool Wait(Context* ctx)
    {
        m_completion.Wait(ctx); return m_spawnOk; }
};

} // end namespace coop
