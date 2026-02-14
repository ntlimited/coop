#pragma once

#include <functional>

#include "coop/cooperator.h"
#include "coop/thread.h"

namespace test
{

// Run a test function inside a cooperator. The function receives the Context* and can use all
// cooperative APIs. The cooperator shuts down automatically when the function returns.
//
// GTest assertions work inside the function because Thread::~Thread joins, establishing a
// happens-before relationship with the calling test.
//
inline void RunInCooperator(std::function<void(coop::Context*)> fn)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        auto* fn = static_cast<std::function<void(coop::Context*)>*>(arg);
        (*fn)(ctx);
        ctx->GetCooperator()->Shutdown();
    }, &fn);
}

} // end namespace test
