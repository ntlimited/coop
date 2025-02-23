#include "coop/time/ticker.h"

namespace coop
{

namespace time
{

Ticker::Ticker(int resolution /* = 0 */)
: m_context(nullptr)
, m_epoch(0)
, m_resolution(resolution)
{
}

void Ticker::Launch(Context* ctx)
{
    m_context = ctx;

    // Epoch is the arbitrary time the last full bucket cycle completed, which starts at the
    // current time when the ticker starts and moves forward if (which would take a long time)
    // we spend more than 1^BUCKETS * INTERVAL time alive.
    //
    for (int i = 0 ; i < BUCKETS ; i++)
    {
        m_buckets[i].lastChecked = 0;
    }
    
    m_epoch = Now();
    while (!ctx->IsKilled())
    {
        ctx->Yield();
        size_t now = Now();
        if (m_epoch == now)
        {
            continue;
        }

        m_epoch = now;

        // We do not process bucket 0 because this phase will move items into bucket 0 for us to
        // handle afterwards
        //
        for (int i = 1 ; i < BUCKETS ; i++)
        {
            if (!ProcessBucket(i, now))
            {
                break;
            }
        }

        // Process has finished populating this bucket with "execute immediately" events
        //

        m_buckets[0].list.Visit([this, ctx](Handle* handle)
        {
            // Remove the handle before switching so that it can get re-submitted if the caller
            // code needs to do so
            //
            m_buckets[0].list.Remove(handle);
            handle->Deadline(ctx);

            return true;
        });
    }
}

bool Ticker::Accept(Handle* handle)
{
    // The handle knows its interval to trigger at, but we need to set a deadline based on 'now'
    //
    auto deadline = handle->SetDeadline(m_epoch, m_resolution);
    auto bucket = BucketFor(deadline);

    m_buckets[bucket].list.Push(handle);
    handle->GetCoordinator()->Acquire(m_context);
    return true;
}


// ProcessBucket handles the current bucket of deadlines to process, moving any deadlines that no
// longer fall in it into the appropriate bucket, and otherwise leaving them be. It returns true
// if it actually was necessary to process the bucket; if the metadata ensures it is not necessary
// to do so, then false is returned. Because each bucket needs to be collected 2x as often as the
// prior one, when we go highest (soonest) to lowest (least frequent) bucket, we can stop processing
// once the first bucket doesn't need any work.
//
bool Ticker::ProcessBucket(int idx, size_t now)
{
    auto& bucket = m_buckets[idx];

    // Per the docs, e.g. bucket 2 stores events between 2 and 4 intervals in the future. If it was
    // last checked less than (1 << (2 - 1)) intervals ago, then we skip checking it for now.
    //
    if ((now - bucket.lastChecked) < (1 << (idx - 1)))
    {
        return false;
    }
    bucket.lastChecked = now;

    bucket.list.Visit([this, idx, now, &bucket](Handle* handle) -> bool
    {
        // Moving to bucket 0 means we'll immediately execute it once we get there; we could
        // handle it immediately but this should also mean that we prioritize tasks that had
        // tighter deadlines (will be ahead in the  
        //
        if (handle->GetDeadline() < now)
        {
            bucket.list.Remove(handle);
            m_buckets[0].list.Push(handle);
            return true;
        }

        // Note that this is unsigned math but we checked for the other case above.
        //
        auto moveTo = BucketFor(handle->GetDeadline() - now);
        if (idx == moveTo)
        {
            return true;
        }

        assert(moveTo < idx);

        bucket.list.Remove(handle);
        m_buckets[moveTo].list.Push(handle);

        // Keep iterating
        //
        return true;
    });

    return true;
}

size_t Ticker::Now() const
{
    return std::chrono::duration_cast<Interval>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count() >> m_resolution;
}

int Ticker::BucketFor(size_t interval) const
{
    auto b = ((sizeof(interval)<<3) - __builtin_clzl(interval)) - m_resolution;
    if (b >= BUCKETS)
    {
        b = BUCKETS - 1;
    }
    return b;
}

} // end namespace coop::time
} // end namespace coop
