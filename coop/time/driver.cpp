#include "coop/time/driver.h"

namespace coop
{

namespace time
{


Driver::Driver()
: m_context(nullptr)
{
}

void Driver::Launch(Context* ctx)
{
    m_context = ctx;

    // Epoch is the arbitrary time the last full bucket cycle completed, which starts at the
    // current time when the driver starts and moves forward if (which would take a long time)
    // we spend more than 1^BUCKETS * INTERVAL time alive.
    //
    m_epoch = Now();

    // Last is the delta after the epoch that we last ran 
    size_t last = m_epoch;

    while (!ctx->IsKilled())
    {
        ctx->Yield();
        size_t now = Now();
        size_t delta = now ^ last;
        last = now;
        if (!delta)
        {
            continue;
        }

        // Get the highest set bit on the xor delta
        //
        int maxBucket = (sizeof(delta)<<3) - __builtin_clzl(delta);
        if (maxBucket >= BUCKETS)
        {
            maxBucket = BUCKETS - 1;
        }

        printf("Processing bucket %d down\n", maxBucket);

        while (maxBucket)
        {
            ProcessBucket(maxBucket--, now);
        }

        m_buckets[0].Visit([this, ctx](Handle* handle)
        {
            // Remove the handle before switching so that it can get re-submitted if the caller
            // code needs to do so
            //
            m_buckets[0].Remove(handle);
            handle->Deadline(ctx);

            return true;
        });
    }
}

bool Driver::Accept(Handle* handle)
{
    // The handle knows its interval to trigger at, but we need to set a deadline based on 'now'
    //
    auto bucket = BucketFor(handle->SetDeadline(m_epoch));
    printf("Placing timer in bucket %d\n", bucket);
    m_buckets[bucket].Push(handle);
    handle->GetCoordinator()->Acquire(m_context);
    return true;
}

void Driver::ProcessBucket(int idx, size_t now)
{
    auto& bucket = m_buckets[idx];

    bucket.Visit([this, idx, now, &bucket](Handle* handle) -> bool
    {
        // Moving to bucket 0 means we'll immediately execute it once we get there; we could
        // handle it immediately but this should also mean that we prioritize tasks that had
        // tighter deadlines (will be ahead in the  
        //
        if (handle->GetDeadline() < now)
        {
            bucket.Remove(handle);
            m_buckets[0].Push(handle);
            return true;
        }

        // Note that this is unsigned math but we checked for the other case above.
        //
        auto moveTo = BucketFor(handle->GetDeadline() - now);
        if (idx == moveTo)
        {
            return true;
        }

        bucket.Remove(handle);
        m_buckets[idx].Push(handle);

        // Keep iterating
        //
        return true;
    });
}

size_t Driver::Now() const
{
    return std::chrono::duration_cast<Interval>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

int Driver::BucketFor(size_t interval) const
{
    printf("Computing bucket for interval %lu\n", interval);
    auto b = (sizeof(interval)<<3) - __builtin_clzl(interval);
    if (b >= BUCKETS)
    {
        b = BUCKETS - 1;
    }
    return b;
}

} // end namespace coop::time
} // end namespace coop
