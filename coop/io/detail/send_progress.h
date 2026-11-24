#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "coop/context.h"
#include "coop/time/now.h"
#include "coop/time/interval.h"
#include "yield_budget.h"

namespace coop
{
namespace io
{
namespace detail
{

// A slow reader may keep a transfer alive by accepting bytes, but cannot extend its
// context's deadline. Only completed socket writes refresh this inactivity budget.
//
struct SendProgress
{
    static constexpr size_t MAX_WRITE = 16384;

    SendProgress(Context* ctx, time::Interval timeout, size_t* sinceYield = nullptr)
    : m_ctx(ctx)
    , m_timeout(timeout)
    , m_lastProgress(time::MonotonicMicros())
    , m_sinceYield(sinceYield)
    {}

    int Prepare(time::Interval& timeout)
    {
        if (m_ctx && m_ctx->IsKilled()) return -ECANCELED;
        int64_t remaining = std::numeric_limits<int64_t>::max();
        if (m_ctx && m_ctx->DeadlineUs() != 0 && m_ctx->RemainingUs() <= 0)
        {
            m_ctx->KillSelf(KillCause::Deadline);
            return -ECANCELED;
        }
        // WaitKill enforces a future context deadline and records its cause. A
        // competing socket timeout at that same instant could hide that cause.
        //
        if (m_timeout.count() > 0)
        {
            int64_t idle = m_timeout.count() - (time::MonotonicMicros() - m_lastProgress);
            if (idle <= 0) return -ETIMEDOUT;
            remaining = std::min(remaining, idle);
        }
        timeout = remaining == std::numeric_limits<int64_t>::max()
            ? time::Interval{} : time::Interval(remaining);
        return 0;
    }

    void Progress(size_t bytes)
    {
        m_lastProgress = time::MonotonicMicros();
        if (m_ctx && m_sinceYield)
        {
            *m_sinceYield += bytes;
            if (*m_sinceYield >= YieldBudget::kDefaultBytes)
            {
                *m_sinceYield = 0;
                m_ctx->Yield(true);
            }
        }
    }

  private:
    Context* m_ctx;
    time::Interval m_timeout;
    int64_t m_lastProgress;
    size_t* m_sinceYield;
};

} // namespace coop::io::detail
} // namespace coop::io
} // namespace coop
