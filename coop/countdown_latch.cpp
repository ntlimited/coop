#include "coop/countdown_latch.h"

#include "coop/cooperator.h"
#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/signal.h"

namespace coop
{
// Hosted on the active wait frame, so unused latches carry only count and pointer.
struct CountdownLatch::Notification : SubmissionEntry
{
    Cooperator* owner;
    Signal signal{nullptr};
    explicit Notification(Cooperator* co) : owner(co)
    {
        m_inline = true;
        m_invoke = [](SubmissionEntry* entry, Context* ctx)
        { static_cast<Notification*>(entry)->signal.Notify(ctx, false); };
        m_destroy = [](SubmissionEntry*) {};
    }
};

void CountdownLatch::Reset(uint32_t count)
{
    assert(!m_registration && count < kWaiter);
    m_remaining.store(count, std::memory_order_release);
}

bool CountdownLatch::IsReady() const
{
    return (m_remaining.load(std::memory_order_acquire) & ~kWaiter) == 0;
}

void CountdownLatch::Arrive()
{
    uint32_t const previous = m_remaining.fetch_sub(1, std::memory_order_acq_rel);
    assert((previous & ~kWaiter) != 0);
    // Without a registered waiter, this RMW is our final access to the latch.
    if (previous != (kWaiter | 1))
        return;
    // Wait publishes this pointer before setting kWaiter. Enqueue also rings the
    // shared doorbell under the queue lock: consumption cannot outrun the wake.
    Notification* notification = m_registration;
    notification->owner->PushSubmission(notification, true);
}

CountdownLatch::WaitStatus CountdownLatch::WaitImpl(Context* ctx, bool killAware,
                                                    void (*onCancel)(void*, WaitStatus),
                                                    void* state, void (*afterRegistration)(void*))
{
    assert(ctx->GetCooperator() == Cooperator::thread_cooperator && !m_registration);
    // Already-completed and empty rounds need neither a notification nor a reader lease.
    if (IsReady())
    {
        WaitStatus const status =
            killAware && ctx->IsKilled() ? WaitStatus::Killed : WaitStatus::Completed;
        if (status != WaitStatus::Completed && onCancel)
            onCancel(state, status);
        return status;
    }
    Cooperator* owner = ctx->GetCooperator();
    Notification notification(owner);
    m_registration = &notification;
    owner->BeginCompletionWait();
    uint32_t const previous = m_remaining.fetch_or(kWaiter, std::memory_order_acq_rel);
    assert(!(previous & kWaiter)); // one pending wait per round
    if ((previous & ~kWaiter) && afterRegistration)
        afterRegistration(state);
    WaitStatus status = WaitStatus::Completed;
    if (killAware && ctx->IsKilled())
        status = WaitStatus::Killed;
    else if ((previous & ~kWaiter) && killAware)
    {
        auto const result = CoordinateWithKill(ctx, &notification.signal);
        if (result.Killed())
            status = WaitStatus::Killed;
        else if (result.Error())
            status = WaitStatus::Error;
    }
    // Deadline coordination retires a timer before returning and that cleanup
    // can suspend after selecting completion. Preserve a kill delivered there.
    if (status == WaitStatus::Completed && killAware && ctx->IsKilled())
        status = WaitStatus::Killed;
    if (status != WaitStatus::Completed && onCancel)
        onCancel(state, status);
    if (previous & ~kWaiter)
        notification.signal.Wait(ctx);
    owner->EndCompletionWait();
    m_registration = nullptr;
    return status;
}

void CountdownLatch::Wait(Context* ctx)
{
    (void)WaitImpl(ctx, false, nullptr, nullptr, nullptr);
}

CountdownLatch::WaitStatus CountdownLatch::WaitKill(Context* ctx,
                                                    void (*onCancel)(void*, WaitStatus),
                                                    void* state, void (*afterRegistration)(void*))
{
    return WaitImpl(ctx, true, onCancel, state, afterRegistration);
}
} // namespace coop
