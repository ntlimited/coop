#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/perf/counters.h"
#include "coop/perf/patch.h"

#include "test_helpers.h"

#if COOP_PERF_MODE == 1

TEST(PerfTest, AlwaysOnCounters)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto& perf = ctx->GetCooperator()->GetPerfCounters();

        // Our context was spawned via EnterContext (from Submit path), which increments
        // ContextSpawn but not ContextResume. Verify spawn was counted.
        //
        EXPECT_GT(perf.Get(coop::perf::Counter::ContextSpawn), 0u);

        // Yield to generate ContextResume and scheduler loop activity.
        //
        ctx->Yield();
        ctx->Yield();

        EXPECT_GT(perf.Get(coop::perf::Counter::ContextResume), 0u);
        EXPECT_GT(perf.Get(coop::perf::Counter::PollCycle), 0u);
        EXPECT_GT(perf.Get(coop::perf::Counter::SchedulerLoop), 0u);
        EXPECT_GT(perf.Get(coop::perf::Counter::ContextYield), 0u);
    });
}

#elif COOP_PERF_MODE == 2

TEST(PerfTest, DynamicPatchToggle)
{
    // Before enabling, probe count should be discoverable but counters should be zero.
    //
    EXPECT_GT(coop::perf::ProbeCount(), 0u);
    EXPECT_FALSE(coop::perf::IsEnabled());

    test::RunInCooperator([](coop::Context* ctx)
    {
        auto& perf = ctx->GetCooperator()->GetPerfCounters();

        // Probes are disabled — counters should be zero despite scheduler activity.
        //
        EXPECT_EQ(perf.Get(coop::perf::Counter::ContextResume), 0u);

        // Enable probes at runtime.
        //
        coop::perf::Enable();
        EXPECT_TRUE(coop::perf::IsEnabled());

        // Yield to generate some counter activity.
        //
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();

        EXPECT_GT(perf.Get(coop::perf::Counter::ContextResume), 0u);
        EXPECT_GT(perf.Get(coop::perf::Counter::ContextYield), 0u);
        EXPECT_GT(perf.Get(coop::perf::Counter::PollCycle), 0u);
        EXPECT_GT(perf.Get(coop::perf::Counter::SchedulerLoop), 0u);

        // Disable probes and verify counters stop incrementing.
        //
        uint64_t resumesBefore = perf.Get(coop::perf::Counter::ContextResume);
        coop::perf::Disable();

        ctx->Yield();
        ctx->Yield();

        EXPECT_EQ(perf.Get(coop::perf::Counter::ContextResume), resumesBefore);
    });
}

#else

TEST(PerfTest, DisabledMode)
{
    // In disabled mode, PerfCounters is an empty struct. Just verify it compiles.
    //
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto& perf = ctx->GetCooperator()->GetPerfCounters();
        (void)perf;
        SUCCEED();
    });
}

#endif

// ---- Multi-cooperator tests (mode-independent) ----

TEST(PerfTest, CooperatorName)
{
    coop::CooperatorConfiguration config = {
        .name = "test-cooperator",
    };
    coop::Cooperator cooperator(config);
    EXPECT_STREQ(cooperator.GetName(), "test-cooperator");
}

TEST(PerfTest, CooperatorNameDefault)
{
    coop::Cooperator cooperator;
    EXPECT_STREQ(cooperator.GetName(), "");
}

TEST(PerfTest, VisitRegistryMultipleCooperators)
{
    constexpr int N = 3;
    const char* names[] = {"worker-0", "worker-1", "worker-2"};

    coop::CooperatorConfiguration configs[N] = {
        {.uring = coop::io::s_defaultUringConfiguration, .name = names[0]},
        {.uring = coop::io::s_defaultUringConfiguration, .name = names[1]},
        {.uring = coop::io::s_defaultUringConfiguration, .name = names[2]},
    };
    coop::Cooperator cooperators[N] = {
        coop::Cooperator(configs[0]),
        coop::Cooperator(configs[1]),
        coop::Cooperator(configs[2]),
    };

    std::vector<std::unique_ptr<coop::Thread>> threads;
    std::atomic<int> running{0};

    for (int i = 0; i < N; i++)
    {
        threads.emplace_back(std::make_unique<coop::Thread>(&cooperators[i]));
        cooperators[i].Submit([](coop::Context* ctx, void* arg)
        {
            auto* running = static_cast<std::atomic<int>*>(arg);
            running->fetch_add(1, std::memory_order_relaxed);
            while (!ctx->IsKilled())
            {
                ctx->Yield(true);
            }
        }, &running);
    }

    while (running.load(std::memory_order_relaxed) < N)
    {
        std::this_thread::yield();
    }

    // VisitRegistry should see all N cooperators
    //
    int count = 0;
    bool foundNames[N] = {};
    coop::Cooperator::VisitRegistry([&](coop::Cooperator* co) -> bool
    {
        for (int i = 0; i < N; i++)
        {
            if (co == &cooperators[i])
            {
                foundNames[i] = true;
            }
        }
        count++;
        return true;
    });

    EXPECT_GE(count, N);
    for (int i = 0; i < N; i++)
    {
        EXPECT_TRUE(foundNames[i]) << "cooperator " << names[i] << " not found in registry";
    }

    coop::Cooperator::ShutdownAll();
    threads.clear();
    coop::Cooperator::ResetGlobalShutdown();
}
