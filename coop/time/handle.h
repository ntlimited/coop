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
// 
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

    // Because this happens in a context, it's safe to unilaterally do this apart from that the
    // accounting for the list gets messed up. We could just record the list in a separate
    // field but not a big deal at this instant.
    //
    ~Handle();

    // Submit the handle to trigger in the future. The native ticker for the cooperator running the
    // context (which must be active, of course) will be used if one is not specified.
    //
    // This is also just sugar for ticker->Accept(handle), but it also allows just using the
    // current 'native' ticker.
    //
    void Submit(Ticker* ticker = nullptr);

    // This is just "sugar" for acquiring the submitted coordinator. It may be more useful in the
    // future though?
    //
    // Also should have a better way of specifying "this is the current context always"
    //
    void Wait(Context* ctx);
    
    // Cancel the handle if it is still submitted.
    //
    void Cancel();

  private:
    friend Ticker;
    
    // Deadline is invoked when the deadline is hit in the ticker
    //
    void Deadline(Context* ctx);

    size_t SetDeadline(size_t epoch, int resolution);

    size_t GetDeadline() const;

    Coordinator* GetCoordinator();

  private:
    Interval m_interval;
    Coordinator* m_coordinator;
    size_t m_deadline;
    List* m_list;
};

} // end namespace coop::time
} // end namespace coop
