#include <cstdio>
#include <cstring>

#include "coop/cooperator.h"
#include "coop/perf/patch.h"
#include "coop/perf/probe.h"
#include "spdlog/common.h"

static_assert(COOP_DEFAULT_STACK_SIZE == 262144);
static_assert(COOP_PERF_MODE == 2);
static_assert(SPDLOG_ACTIVE_LEVEL == 3);
static_assert(coop::perf::CounterFamily(coop::perf::Counter::ConsumerWork)
    == coop::perf::Family::Consumer);

namespace
{

// Reuse one probe site across toggles so this also checks the library's family mapping.
//
__attribute__((noinline)) void ConsumerWork(coop::perf::Counters& counters)
{
    COOP_PERF_INC(counters, coop::perf::Counter::ConsumerWork);
}

bool Check(bool condition, char const* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

} // namespace

int main()
{
    using namespace coop::perf;
    bool passed = true;
    passed &= Check(std::strcmp(CounterName(Counter::ConsumerWork), "consumer_work") == 0,
        "consumer counter metadata is missing");
    passed &= Check(std::strcmp(FamilyName(Family::Consumer), "consumer") == 0,
        "consumer family metadata is missing");

    Counters counters{};
    Disable();
    ConsumerWork(counters);
    Enable(Family::Scheduler);
    ConsumerWork(counters);
    passed &= Check(counters.Get(Counter::ConsumerWork) == 0,
        "consumer probe enabled with an unrelated family");
    Enable(Family::Consumer);
    ConsumerWork(counters);
    passed &= Check(counters.Get(Counter::ConsumerWork) == 1,
        "consumer probe did not increment after enabling its family");
    Disable(Family::Consumer);
    ConsumerWork(counters);
    passed &= Check(counters.Get(Counter::ConsumerWork) == 1,
        "consumer probe incremented after disabling its family");
    Disable();

    // The submission drainer is spawned inside the compiled runtime. Its stack verifies
    // that consumer settings reach runtime compilation as well as consumer headers.
    //
    coop::Cooperator cooperator;
    bool state[] = {false, true};
    bool submitted = cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        auto* state = static_cast<bool*>(arg);
        state[0] = true;
        bool foundDrainer = false;
        auto* co = ctx->GetCooperator();
        state[1] &= Check(ctx->m_segment.Size() == COOP_DEFAULT_STACK_SIZE,
            "submitted context has the wrong stack size");
        co->VisitContexts([&](coop::Context* other)
        {
            if (std::strcmp(other->GetName(), "SubmissionDrainer") == 0)
            {
                foundDrainer = true;
                state[1] &= Check(other->m_segment.Size() == COOP_DEFAULT_STACK_SIZE,
                    "runtime default stack size differs from the consumer");
            }
            return true;
        });
        state[1] &= Check(foundDrainer, "runtime submission drainer was not found");
        co->Shutdown();
    }, state);
    if (!submitted)
    {
        return 1;
    }
    cooperator.Launch();
    passed &= state[1];
    passed &= Check(state[0], "consumer context did not run");
    return passed ? 0 : 1;
}
