#include "coop/cooperate_submission.h"

#include "coop/cooperator.h"
#include "coop/context.h"
#include "coop/detail/memory_order.h"

#include <cstdio>
#include <cstdlib>

namespace coop
{
CooperateSubmission::Cancellation::Cancellation(CooperateSubmission* submission)
: owner(submission)
{
    m_inline = true;
    m_invoke = [](SubmissionEntry* entry, Context*)
    { static_cast<Cancellation*>(entry)->owner->DeliverCancellation(); };
    m_destroy = [](SubmissionEntry*) {};
}

CooperateSubmission::CooperateSubmission(Body body, void* state, Completion completion,
                                        SpawnConfiguration const& config)
: m_body(body), m_state(state), m_onComplete(completion)
{
    assert(body);
    m_callerOwned = true;
    m_config = config;
}

CooperateSubmission::~CooperateSubmission()
{
    if (!IsRetired())
    {
        std::fputs("caller-owned submission destroyed before retirement\n", stderr);
        std::abort();
    }
}

bool Cooperator::Cooperate(CooperateSubmission& submission)
{
    {
        std::lock_guard<std::mutex> lock(submission.m_lock);
        if (submission.GetStatus() != CooperateSubmission::Status::Idle)
            return false;
        submission.m_target = this;
        submission.m_status.store(CooperateSubmission::Status::Queued, std::memory_order_release);
    }
    if (!PushSubmission(&submission))
    {
        submission.Finish(CooperateSubmission::Status::Refused);
        return false;
    }
    WakeCooperator();
    return true;
}

void CooperateSubmission::Cancel()
{
    std::lock_guard<std::mutex> lock(m_lock);
    auto const status = GetStatus();
    if (status != Status::Idle && status != Status::Queued && status != Status::Running)
        return;
    m_cancelled = true;
    if (status != Status::Running || m_cancelQueued)
        return;

    // Publish an extra borrower before the owner can consume the embedded notification.
    // Once admission closes, the owner's shutdown sweep supplies the running-task kill.
    //
    m_cancelQueued = true;
    ++m_borrowers;
    if (!m_target->PushSubmission(&m_cancellation))
        --m_borrowers;
    else
        m_target->WakeCooperator();
}

void CooperateSubmission::DeliverCancellation()
{
    Context* running;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        running = m_runningContext;
    }
    // This notification runs on the context's owner. Neither the lookup nor KillSelf
    // switches contexts, so the pointer cannot retire between them. Hooks may not block.
    //
    if (running)
        running->KillSelf(KillCause::Kill);
    Release();
}

void CooperateSubmission::Run(Context* ctx)
{
    ctx->m_ownedSubmission = this;
    bool invoke;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        invoke = !m_cancelled && !m_target->m_shutdown.load(detail::kLoadFlag);
        if (invoke)
        {
            m_runningContext = ctx;
            m_status.store(Status::Running, std::memory_order_release);
        }
    }
    if (!invoke)
        return;

    m_body(ctx, m_state);
    // Only the owner can deliver this context's kill. Capture it immediately at body
    // return: a remote cancellation request still queued at that point is not an outcome.
    // Freeze before joining children, whose teardown may yield and receive a later kill.
    Status const result = ctx->IsKilled() ? Status::Killed : Status::Completed;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_result = result;
    }
    // Match context retirement: cancel remaining attached trees before borrowed state ends.
    ctx->JoinChildren(KillCause::Kill);
}

void CooperateSubmission::Retired()
{
    Status result;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_runningContext = nullptr;
        result = m_result;
    }
    Finish(result);
}

void CooperateSubmission::Finish(Status result)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_result = result == Status::Refused && m_cancelled ? Status::Cancelled : result;
    }
    Release();
}

void CooperateSubmission::Release()
{
    Completion completion;
    void* state;
    Status result;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        assert(m_borrowers);
        if (--m_borrowers)
            return;
        result = m_result;
        m_status.store(result, std::memory_order_release);
        completion = m_onComplete;
        state = m_state;
    }
    if (completion)
        completion(state, result);
    // Last access: another cooperator may retire this object as soon as Arrive publishes.
    m_done.Arrive();
}

CooperateSubmission::Status CooperateSubmission::Wait(Context* ctx)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (GetStatus() == Status::Idle)
            return Status::Idle;
        if (m_waiting.exchange(true, std::memory_order_acq_rel))
        {
            std::fputs("caller-owned submission permits one cooperative waiter\n", stderr);
            std::abort();
        }
    }
    m_done.WaitKill(ctx, [](void* state, CountdownLatch::WaitStatus)
    { static_cast<CooperateSubmission*>(state)->Cancel(); }, this);
    Status const result = GetStatus();
    // Final slot access; a plain-thread successor may retire it after this publication.
    m_waiting.store(false, std::memory_order_release);
    return result;
}

bool CooperateSubmission::IsRetired() const
{
    return GetStatus() == Status::Idle ||
        (m_done.IsReady() && !m_waiting.load(std::memory_order_acquire));
}

CooperateSubmission::Status CooperateSubmission::CancelAndWait(Context* ctx)
{
    Cancel();
    return Wait(ctx);
}
} // namespace coop
