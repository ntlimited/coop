#pragma once

#include <cstddef>

#include "coop/context.h"

namespace coop
{

namespace io
{

namespace detail
{

// YieldBudget: the fairness governor for syscall-loop data paths. Splice and sendfile
// loops only reach the scheduler through Poll on EAGAIN — a peer that never pushes back
// (fast reader, loopback, same-host hop) keeps them succeeding indefinitely, letting one
// bulk transfer hold a cooperative slot for its whole body. Charge() forces a yield
// after each budget of unblocked progress. 2MB is nginx's sendfile_max_chunk lesson
// (same default); at that granularity the scheduler round trip is noise.
//
// The ring-op paths never need this: every blocking uring op already passes through the
// scheduler.
//
struct YieldBudget
{
    static constexpr size_t kDefaultBytes = 2 * 1024 * 1024;

    explicit YieldBudget(Context* ctx, size_t budgetBytes = kDefaultBytes)
    : m_ctx(ctx)
    , m_budget(budgetBytes)
    {
    }

    void Charge(size_t bytes)
    {
        m_since += bytes;
        if (m_since >= m_budget)
        {
            m_since = 0;
            m_ctx->Yield(true);
        }
    }

private:
    Context* m_ctx;
    size_t   m_budget;
    size_t   m_since{0};
};

} // end namespace coop::io::detail
} // end namespace coop::io
} // end namespace coop
