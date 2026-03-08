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

TEST(PerfTest, FamilySelectiveEnable)
{
    // Enable only Scheduler family — IO counters should remain zero.
    //
    coop::perf::Disable();
    EXPECT_FALSE(coop::perf::IsEnabled());

    test::RunInCooperator([](coop::Context* ctx)
    {
        auto& perf = ctx->GetCooperator()->GetPerfCounters();
        perf.Reset();

        // Enable only Scheduler family.
        //
        coop::perf::Enable(coop::perf::Family::Scheduler);
        EXPECT_TRUE(coop::perf::IsEnabled());
        EXPECT_TRUE(coop::perf::HasFamily(
            coop::perf::EnabledFamilies(), coop::perf::Family::Scheduler));
        EXPECT_FALSE(coop::perf::HasFamily(
            coop::perf::EnabledFamilies(), coop::perf::Family::IO));

        ctx->Yield();
        ctx->Yield();

        // Scheduler counters should fire.
        //
        EXPECT_GT(perf.Get(coop::perf::Counter::ContextResume), 0u);
        EXPECT_GT(perf.Get(coop::perf::Counter::ContextYield), 0u);

        // IO counters should not fire (PollCycle is IO family).
        //
        EXPECT_EQ(perf.Get(coop::perf::Counter::PollCycle), 0u);
        EXPECT_EQ(perf.Get(coop::perf::Counter::PollSubmit), 0u);

        // Now additionally enable IO.
        //
        coop::perf::Enable(coop::perf::Family::IO);
        EXPECT_TRUE(coop::perf::HasFamily(
            coop::perf::EnabledFamilies(), coop::perf::Family::IO));

        ctx->Yield();

        EXPECT_GT(perf.Get(coop::perf::Counter::PollCycle), 0u);

        // Disable just Scheduler — IO should continue.
        //
        uint64_t pollBefore = perf.Get(coop::perf::Counter::PollCycle);
        uint64_t resumeBefore = perf.Get(coop::perf::Counter::ContextResume);
        coop::perf::Disable(coop::perf::Family::Scheduler);

        ctx->Yield();

        EXPECT_EQ(perf.Get(coop::perf::Counter::ContextResume), resumeBefore);
        EXPECT_GT(perf.Get(coop::perf::Counter::PollCycle), pollBefore);

        coop::perf::Disable();
    });
}

TEST(PerfTest, SetFamilies)
{
    coop::perf::Disable();

    test::RunInCooperator([](coop::Context* ctx)
    {
        auto& perf = ctx->GetCooperator()->GetPerfCounters();
        perf.Reset();

        // SetFamilies atomically replaces the enabled mask.
        //
        coop::perf::SetFamilies(
            coop::perf::Family::Scheduler | coop::perf::Family::IO);
        EXPECT_TRUE(coop::perf::HasFamily(
            coop::perf::EnabledFamilies(), coop::perf::Family::Scheduler));
        EXPECT_TRUE(coop::perf::HasFamily(
            coop::perf::EnabledFamilies(), coop::perf::Family::IO));

        ctx->Yield();

        EXPECT_GT(perf.Get(coop::perf::Counter::ContextResume), 0u);
        EXPECT_GT(perf.Get(coop::perf::Counter::PollCycle), 0u);

        // Switch to IO-only.
        //
        uint64_t resumeBefore = perf.Get(coop::perf::Counter::ContextResume);
        coop::perf::SetFamilies(coop::perf::Family::IO);

        ctx->Yield();

        EXPECT_EQ(perf.Get(coop::perf::Counter::ContextResume), resumeBefore);
        EXPECT_FALSE(coop::perf::HasFamily(
            coop::perf::EnabledFamilies(), coop::perf::Family::Scheduler));

        coop::perf::Disable();
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

// ---- Counter family infrastructure tests (mode-independent) ----

TEST(PerfTest, CounterFamilyMapping)
{
    using C = coop::perf::Counter;
    using F = coop::perf::Family;

    // Spot-check representative counters from each family.
    //
    EXPECT_EQ(coop::perf::CounterFamily(C::SchedulerLoop), F::Scheduler);
    EXPECT_EQ(coop::perf::CounterFamily(C::ContextResume), F::Scheduler);
    EXPECT_EQ(coop::perf::CounterFamily(C::IoSubmit), F::IO);
    EXPECT_EQ(coop::perf::CounterFamily(C::PollCqe), F::IO);
    EXPECT_EQ(coop::perf::CounterFamily(C::EpochAdvance), F::Epoch);
    EXPECT_EQ(coop::perf::CounterFamily(C::DrainReclaimed), F::Epoch);
}

TEST(PerfTest, FamilyNames)
{
    using F = coop::perf::Family;
    EXPECT_STREQ(coop::perf::FamilyName(F::Scheduler), "scheduler");
    EXPECT_STREQ(coop::perf::FamilyName(F::IO), "io");
    EXPECT_STREQ(coop::perf::FamilyName(F::Epoch), "epoch");
}

TEST(PerfTest, CounterNames)
{
    using C = coop::perf::Counter;
    // Verify new counter names are non-null and match expected strings.
    //
    EXPECT_STREQ(coop::perf::CounterName(C::EpochAdvance), "epoch_advance");
    EXPECT_STREQ(coop::perf::CounterName(C::DrainReclaimed), "drain_reclaimed");
}

TEST(PerfTest, FamilyBitmaskOps)
{
    using F = coop::perf::Family;

    F combo = F::Scheduler | F::IO;
    EXPECT_TRUE(coop::perf::HasFamily(combo, F::Scheduler));
    EXPECT_TRUE(coop::perf::HasFamily(combo, F::IO));

    EXPECT_TRUE(coop::perf::HasFamily(F::All, F::Scheduler));
    EXPECT_TRUE(coop::perf::HasFamily(F::All, F::Epoch));
}

TEST(PerfTest, AllCountersHaveFamily)
{
    // Every counter should be assigned to a specific family (not All).
    //
    for (uint32_t i = 0; i < static_cast<uint32_t>(coop::perf::Counter::COUNT); i++)
    {
        auto c = static_cast<coop::perf::Counter>(i);
        coop::perf::Family f = coop::perf::CounterFamily(c);
        EXPECT_NE(f, coop::perf::Family::All)
            << "Counter " << coop::perf::CounterName(c) << " has no family assignment";
    }
}

// ---- Multi-cooperator tests (mode-independent) ----

TEST(PerfTest, CooperatorName)
{
    coop::CooperatorConfiguration config;
    config.SetName("test-cooperator");
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

    coop::CooperatorConfiguration configs[N];
    for (int i = 0; i < N; i++) configs[i].SetName(names[i]);
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
