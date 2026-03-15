#pragma once

#include "signal.h"

namespace coop
{

struct Context;

// CooperateHandle answers one question: did the dispatched work successfully spawn on the target
// cooperator? It does NOT signal work completion — that is the caller's responsibility via
// whatever mechanism they choose (Signal, Coordinator, Passage, shared atomics).
//
struct CooperateHandle
{
    Signal   m_signal;
    bool     m_spawnOk{false};

    explicit CooperateHandle(Context* owner) : m_signal(owner) {}

    // Cooperative wait. Returns spawn success.
    //
    bool Wait(Context* ctx) { m_signal.Wait(ctx); return m_spawnOk; }
};

} // end namespace coop
