#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/prng.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "coop/time/sleep.h"

// Same seed, same stream — the whole foundation.
//
TEST(PrngTest, DeterministicSequence)
{
    coop::Prng a = coop::Prng::Seeded(42);
    coop::Prng b = coop::Prng::Seeded(42);
    for (int i = 0; i < 1000; i++) ASSERT_EQ(a.Next(), b.Next());

    coop::Prng c = coop::Prng::Seeded(43);
    coop::Prng d = coop::Prng::Seeded(42);
    bool diverged = false;
    for (int i = 0; i < 8; i++) diverged |= (c.Next() != d.Next());
    EXPECT_TRUE(diverged);
}

TEST(PrngTest, BelowStaysInRange)
{
    coop::Prng p = coop::Prng::Seeded(7);
    for (int i = 0; i < 10000; i++)
    {
        uint64_t v = p.Below(100);
        ASSERT_LT(v, 100u);
    }
    EXPECT_EQ(p.Below(0), 0u);
}

// A configured seed is used verbatim and reported; seed 0 draws a distinct one from entropy.
//
TEST(PrngTest, CooperatorSeeding)
{
    coop::CooperatorConfiguration cfg;
    cfg.rngSeed = 123456;
    coop::Cooperator a(cfg);
    coop::Cooperator b(cfg);
    EXPECT_EQ(a.Seed(), 123456u);
    for (int i = 0; i < 100; i++) ASSERT_EQ(a.Rng().Next(), b.Rng().Next());

    coop::CooperatorConfiguration entropy;   // rngSeed defaults to 0
    coop::Cooperator c(entropy);
    EXPECT_NE(c.Seed(), 0u);
}

namespace
{

// Run a little "simulation": N workers each sleep a random (rng-drawn) virtual duration, then record
// their id. Under virtual time the wakeups happen in deadline order and instantly; the drawn
// durations — hence the whole schedule — are a pure function of the seed. Returns the wake order.
//
std::vector<int> RunSim(uint64_t seed)
{
    coop::CooperatorConfiguration cfg;
    cfg.virtualTime = true;
    cfg.rngSeed = seed;

    coop::Cooperator co(cfg);
    coop::Thread thread(&co);

    std::vector<int> order;
    co.SubmitSync([&](coop::Context* ctx)
    {
        auto* c = ctx->GetCooperator();
        for (int i = 0; i < 16; i++)
        {
            int delayMs = static_cast<int>(1 + c->Rng().Below(1000));
            c->Spawn([&order, i, delayMs](coop::Context* w)
            {
                coop::time::Sleep(w, std::chrono::milliseconds(delayMs));
                order.push_back(i);
            });
        }
        coop::time::Sleep(ctx, std::chrono::seconds(10));   // past every worker
        c->Shutdown();
    });

    return order;
}

} // namespace

// The DST property: a seeded, virtual-time cooperator replays its schedule exactly.
//
TEST(SimTest, SameSeedSameSchedule)
{
    auto run1 = RunSim(0xC0FFEE);
    auto run2 = RunSim(0xC0FFEE);

    ASSERT_EQ(run1.size(), 16u);
    EXPECT_EQ(run1, run2);                    // identical wake order, reproduced from the seed

    auto other = RunSim(0xBADF00D);
    EXPECT_NE(run1, other);                   // a different seed yields a different schedule
}
