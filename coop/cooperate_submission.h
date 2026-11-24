#pragma once

#include <atomic>
#include <mutex>

#include "countdown_latch.h"
#include "detail/submission_entry.h"

namespace coop
{
struct Cooperator;
struct Context;

// Caller-owned, one-shot cross-cooperator work and completion storage. Construct before
// publication and retain the slot and borrowed state through Wait or IsRetired. Keep the
// target alive through completion; destroying an unlaunched target retires its queued submissions.
// An accepted submission may finish before Cooperate returns. Wait joins the body, every
// attached child subtree, context-variable/callable teardown, context retirement, the
// completion callback and any queued cancellation notification.
// Returning from the body cancels remaining attached children, as ordinary context exit does.
// Detached work must not borrow this storage. Retire any concurrent API calls before
// destroying the slot. With no active Wait, any thread may free the slot and borrowed state
// after IsRetired returns true. With a waiter, join that API call before freeing its storage.
// Destruction before retirement terminates the process, including in release builds.
// No library allocation occurs on submission, cancellation or completion; spawning the target
// context retains ordinary stack allocation.
//
class CooperateSubmission : private SubmissionEntry
{
public:
    // Cancelled and Refused guarantee the body was never invoked. Killed means the body
    // was invoked and its context was killed before it returned: effects may have occurred.
    // Completed means the body returned without kill. Its result is frozen at body return;
    // cancellation during child/context cleanup cannot change it.
    enum class Status { Idle, Queued, Running, Completed, Cancelled, Killed, Refused };
    using Body = void (*)(Context*, void*);
    using Completion = void (*)(void*, Status);

    // Body may cooperate but must not throw. Completion must not throw, yield or block;
    // it runs exactly once on the thread retiring the work. Executed work completes on the
    // target owner after worker retirement; Self() may be null or another valid runtime context.
    // Refusal completes on the admission, shutdown or destruction thread. Completion must
    // not free the slot or borrowed state, or signal a successor to free them immediately:
    // the library still accesses the completion latch after the callback. A successor on
    // any thread must observe IsRetired() before freeing, and join concurrent API calls.
    // Refused includes shutdown and failure to spawn, with no body invocation.
    //
    CooperateSubmission(Body body, void* state, Completion completion = nullptr,
                        SpawnConfiguration const& config = s_defaultConfiguration);
    ~CooperateSubmission();
    CooperateSubmission(CooperateSubmission const&) = delete;
    CooperateSubmission& operator=(CooperateSubmission const&) = delete;

    // May run on any thread while target storage remains alive. Cancellation before the
    // target claims Running prevents body invocation; cancellation after that claim requests
    // its context's kill, including during context-variable and child cleanup.
    // Cancel alone does not retire storage. Repeated cancellation is harmless.
    //
    void Cancel();

    // One active cooperative waiter; a second concurrent waiter terminates the process.
    // Waiting on an unsubmitted slot returns Idle immediately. A killed/expired waiter
    // requests cancellation, then joins unconditionally so returning permits retirement.
    // Work must observe its kill; joining has no early timeout that could abandon storage.
    //
    Status Wait(Context* ctx);
    Status CancelAndWait(Context* ctx);

    // Observation only: a terminal status does not replace the retirement boundary.
    Status GetStatus() const { return m_status.load(std::memory_order_acquire); }

    // Thread-safe, nonblocking retirement boundary, including the callback and queued
    // cancellation notifications. False while a cooperative Wait is active. The caller
    // must prevent new API calls and join existing calls before freeing slot/state storage.
    // An Idle slot is already safe to destroy; this probe returns true for it as well.
    bool IsRetired() const;

private:
    friend struct Cooperator;
    friend struct CooperateSubmissionTestAccess;
    struct Cancellation : SubmissionEntry
    {
        CooperateSubmission* owner;
        explicit Cancellation(CooperateSubmission*);
    };

    void Run(Context*);
    void Retired();
    void Finish(Status);
    void Release();
    void DeliverCancellation();

    Body m_body;
    void* m_state;
    Completion m_onComplete;
    std::atomic<Status> m_status{Status::Idle};
    std::atomic<bool> m_waiting{false};
    std::mutex m_lock;
    Cooperator* m_target{nullptr};
    Context* m_runningContext{nullptr};
    bool m_cancelled{false};
    bool m_cancelQueued{false};
    unsigned m_borrowers{1};
    Status m_result{Status::Refused};
    Cancellation m_cancellation{this};
    CountdownLatch m_done{1};
};
} // namespace coop
