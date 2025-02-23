#pragma once

#include <cassert>

#include "interval.h"

#include "coop/coordinator.h"
#include "coop/embedded_list.h"

namespace coop
{

struct Context;
struct Coordinator;

namespace time
{

struct Ticker;

// A coop::time::Handle allows coordinating with a future trigger. Using a handle involves first
// submitting it to a ticker, and then waiting on it. This can fairly easily be strung together
// into all kinds of high level mechanics by creating a bunch of contexts; however, these are
// 'userland' versions of what the time package offers natively.
//
struct Handle : EmbeddedListHookups<Handle>
{
    using List = EmbeddedList<Handle>;

    template<typename I>
    Handle(I interval, Coordinator* coordinator)
    : m_interval(std::chrono::duration_cast<Interval>(interval))
    , m_coordinator(coordinator)
    , m_deadline(0)
    {
        // 0 deadline is not allowed, it's just a fancy Yield
        //
        assert(m_interval != Interval(0));
    }

    // Submit the handle to trigger in the future
    //
    void Submit(Ticker* ticker);

    // Wait for the handle's deadline to bet hit. This contract is slightly looser than ideal,
    // because we don't want to let an un-submitted handle be waited on, but it's also possible
    // that a handle which was satisfied during a yield is waited on after that point and at
    // that time it is not "submitted" anymore. So this is kind of C_I friendly.
    //
    // Note that you also can just Acquire the coordinator directly; as long as you release before
    // you resubmit it to the Ticket, it's fine.
    //
    void Wait(Context* ctx);
    
    // Cancel the handle before it was triggered. Note that this is a delicate contract.
    //
    void Cancel(Context* ctx);

  private:
    friend Ticker;
    
    // Deadline is invoked when the deadline is hit in the ticker
    //
    void Deadline(Context* ctx);

    size_t SetDeadline(size_t epoch);

    size_t GetDeadline() const;

    Coordinator* GetCoordinator();

  private:
    Interval m_interval;
    Coordinator* m_coordinator;
    size_t m_deadline;
};

} // end namespace coop::time
} // end namespace coop
