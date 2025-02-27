#pragma once

#include "handle.h"
#include "interval.h"

#include "coop/context.h"
#include "coop/launchable.h"

namespace coop
{

namespace time
{

// The timing system functions by slotting tasks into buckets based on the duration until the
// task's deadline. Each bucket covers a power-of-two range, e.g.:
//
// * Bucket 0 stores deadlines between [0, 1)
// * Bucket 1 stores deadlines between [1, 2)
// * Bucket 2 stores deadlines between [2, 4)
// * Bucket N stores deadlines between [1<<(N-1), 1<<N)
//
// We can keep the metadata for the last time that we checked a bucket, and then scan from left
// to right each time 'now' changes. As long as we scan bucket 1 every tick, bucket 2 every other,
// and so on based on the left boundary, we will always move events properly (generally one bucket
// at a time).
//
// The unit of operation for Tickers is a Handle, which pairs a deadline in the future with a
// coordinator to be released at or after that moment. This is then submitted to a ticker, and
// the contract is honored with
//
struct Ticker : Launchable
{
    static constexpr int BUCKETS = 32;

    Ticker(int resolution = 0);

    void Launch(Context* ctx) final;
    
    // Submit the given coordinator to be released after `interval` has passed. If submission succeeds,
    // then the coordinator and handle lifetime must exceed when `interval` passes, or until the
    // handle is cancelled.
    //
    // It is expected that the 
    //
    bool Accept(Handle* handle);

  private:
    bool ProcessBucket(int idx, size_t now);

    // `Now` returns the unix-epoch based number of passed 'Intervals' of time.
    //
    size_t Now() const;
    
    // Return the offset to the bucket that a timer for the given interval should go into
    //
    int BucketFor(size_t interval) const;

    // TimeoutList is an embedded list which contexts can only be in one of at once, which logically
    // makes sense that a context would only be in one of, both across tickers and across buckets.
    //
    // Buckets are power-of-two based ranges where bucket 0 corresponds to contexts to unblock
    // within the next 0 - 1 * Interval, bucket 1 from 1 - 2, 2 from 2-4, etc.
    //
    // Each time the ticker awakes, based on the number of [interval] passed, buckets are rotated
    //
    struct {
        Handle::List list;
        size_t lastChecked;
    } m_buckets[BUCKETS];

    Context* m_context;
    size_t m_epoch;
    int m_resolution;
};

} // end namespace coop::time
} // end namespace coop
