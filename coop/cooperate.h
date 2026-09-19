#pragma once

#include "detail/submission_entry.h"
#include "signal.h"

namespace coop
{

struct Context;

// CooperateHandle answers one question: did the dispatched work successfully spawn on the target
// cooperator? It does NOT signal work completion — that is the caller's responsibility via
// whatever mechanism they choose (Signal, Coordinator, Passage, shared atomics).
//
// The reverse notify is embedded here so the target never heap-allocates to wake the caller.
// Wait() must run before this handle is destroyed if Cooperate() returned true with a handle.
//
struct CooperateHandle
{
    Signal   m_signal;
    bool     m_spawnOk{false};
    SubmissionEntry m_reply;

    explicit CooperateHandle(Context* owner)
    : m_signal(owner)
    {
        m_reply.m_inline = true;
        m_reply.m_completionOk = reinterpret_cast<bool*>(this);
        m_reply.m_invoke = [](SubmissionEntry* self, Context* ctx)
        {
            auto* handle = reinterpret_cast<CooperateHandle*>(self->m_completionOk);
            handle->m_signal.Notify(ctx, false);
        };
        m_reply.m_destroy = [](SubmissionEntry*) {};
    }

    // Cooperative wait. Returns spawn success.
    //
    bool Wait(Context* ctx) { m_signal.Wait(ctx); return m_spawnOk; }
};

} // end namespace coop
