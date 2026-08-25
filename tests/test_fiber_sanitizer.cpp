#include <cstdint>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/detail/asan_fiber.h"
#include "coop/self.h"
#include "test_helpers.h"

// These tests exist for what AddressSanitizer says about them, not for what they assert.
//
// ASan tracks one stack per thread. coop gives a thread many, and moves between them in hand-written
// assembly that no instrumentation can see. Left unannotated, ASan's shadow describes whichever
// stack ran last, and the failure that produces is not a missed bug — it is a steady supply of
// invented ones, reported against whatever ordinary function happens to touch an address a departed
// fiber poisoned. A suite in that state cannot be used to find anything.
//
// So the pass condition for most of what follows is "ASan stayed quiet while a lot of switching
// happened". The assertions are there to make the fibers do real work with real stack memory; they
// are not the measurement. The three shapes below are the ones that break first when the
// annotations are missing: deep interleaving, segments returned to the pool and reissued, and the
// waker-to-waiter handoff that bypasses the scheduler loop entirely.
//
// Running them in an ordinary build is not wasted — they are a reasonable scheduler stress — but
// they prove nothing there.

namespace
{

// Instrumented frames and GTest's assertion machinery both want room, and coop's 16KB default is
// sized for fibers that do neither.
//
constexpr coop::SpawnConfiguration kRoomy = {.priority = 0, .stackSize = 128 * 1024};

// Fill a stack array with a checkable pattern, yield, and confirm it survived. The array spans
// several shadow granules and is touched on both sides of the switch, so anything that corrupts or
// mis-poisons this fiber's stack while it is descheduled shows up here.
//
void ChurnStack(coop::Context* ctx, uint64_t seed, int rounds)
{
    volatile uint64_t scratch[64];

    for (int r = 0; r < rounds; r++)
    {
        uint64_t const base = seed * 1000003u + static_cast<uint64_t>(r);

        for (size_t i = 0; i < 64; i++)
        {
            scratch[i] = base + i;
        }

        ctx->Yield(true);

        for (size_t i = 0; i < 64; i++)
        {
            ASSERT_EQ(scratch[i], base + i);
        }
    }
}

} // end anonymous namespace

// Many fibers, deeply interleaved. This drives every switch flavour the scheduler has: direct
// fiber-to-fiber yields, trips back out through the loop, first entries, and final exits.
//
TEST(FiberSanitizerTest, ManySwitchesAcrossFibers)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int kFibers = 16;
        constexpr int kRounds = 32;

        int finished = 0;

        for (int i = 0; i < kFibers; i++)
        {
            ctx->GetCooperator()->Spawn(kRoomy, [&finished, i](coop::Context* child)
            {
                ChurnStack(child, static_cast<uint64_t>(i) + 1, kRounds);
                finished++;
            });
        }

        while (finished < kFibers)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(finished, kFibers);
    });
}

// Spawn and retire far more contexts than StackPool caches, so segments are returned and reissued
// many times over. A recycled segment carries its previous tenant's shadow state unless something
// resets it, and that poison lands underneath the next fiber's locals.
//
TEST(FiberSanitizerTest, RecycledStacksStayClean)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int kGenerations = 40;
        constexpr int kPerGeneration = 8;

        uint64_t checksum = 0;

        for (int g = 0; g < kGenerations; g++)
        {
            int done = 0;

            for (int i = 0; i < kPerGeneration; i++)
            {
                ctx->GetCooperator()->Spawn(kRoomy, [&done, &checksum, g](coop::Context* child)
                {
                    volatile uint64_t frame[96];
                    for (size_t k = 0; k < 96; k++)
                    {
                        frame[k] = static_cast<uint64_t>(g) * 97u + k;
                    }

                    child->Yield(true);

                    uint64_t sum = 0;
                    for (size_t k = 0; k < 96; k++)
                    {
                        sum += frame[k];
                    }
                    checksum += sum;
                    done++;
                });
            }

            while (done < kPerGeneration)
            {
                ctx->Yield(true);
            }
        }

        EXPECT_GT(checksum, 0u);
    });
}

// A context blocked on a coordinator is resumed by the waker's own switch rather than by the
// scheduler loop, which is a distinct path with its own pair of annotations. Each round here blocks
// a fiber with live stack state, hands control to it directly, and checks the state came back.
//
TEST(FiberSanitizerTest, CoordinatorHandoffSwitches)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int kRounds = 48;

        int handoffs = 0;

        for (int r = 0; r < kRounds; r++)
        {
            coop::Coordinator coord(ctx);
            bool resumed = false;

            ctx->GetCooperator()->Spawn(kRoomy, [&](coop::Context* child)
            {
                volatile uint64_t frame[48];
                for (size_t k = 0; k < 48; k++)
                {
                    frame[k] = k * 31u + static_cast<uint64_t>(r);
                }

                coord.Acquire(child);

                for (size_t k = 0; k < 48; k++)
                {
                    ASSERT_EQ(frame[k], k * 31u + static_cast<uint64_t>(r));
                }

                resumed = true;
                coord.Release(child, false);
            });

            // Spawn returned because the child blocked on the held coordinator. Releasing with
            // schedule=true is the handoff: control goes straight from here into the waiter.
            //
            coord.Release(ctx, true);

            EXPECT_TRUE(resumed);
            handoffs++;
        }

        EXPECT_EQ(handoffs, kRounds);
    });
}

#if COOP_HAVE_ASAN

namespace
{

int* volatile g_escaped = nullptr;

// Let a local outlive its frame. With fake stacks on, the address stays valid-looking but ASan owns
// the memory and knows the frame is gone.
//
__attribute__((noinline)) void StashLocalAddress()
{
    int local[8] = {};
    local[0] = 0x5eed;
    g_escaped = &local[0];
}

} // end anonymous namespace

// The calibration case in the other direction: a real error that has to survive a switch to be
// caught at all.
//
// The pointer is taken on a fiber, the fiber is descheduled and the scheduler loop runs, and only
// then is the dead frame read. Catching that requires the fake stack the fiber owned before the
// switch to still be its own afterwards — which is exactly what the BEGIN/END pair carries across.
// A run that reports nothing here is not a clean run; it is a run whose stack shadow stopped meaning
// anything the moment the fiber gave up the CPU.
//
// detect_stack_use_after_return is off by default and is set here for the child only. The threadsafe
// death-test style re-executes this binary to produce that child, which is what gives the option a
// chance to reach a sanitizer at its own startup — an in-process fork would inherit a runtime whose
// mode was already fixed. Confining it to the child also keeps the rest of the suite in the
// configuration everyone else runs it in, which matters because coop has a Handle-lifetime hazard
// that fake stacks turn from latent into fatal (Context::~Context writes through m_handle, and the
// frame that owns the handle may already have returned).
//
TEST(FiberSanitizerDeathTest, UseAfterReturnSurvivesASwitch)
{
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";

    char const* const previous = getenv("ASAN_OPTIONS");
    std::string const restore = previous ? previous : "";
    ASSERT_EQ(setenv("ASAN_OPTIONS", "detect_stack_use_after_return=1", 1), 0);

    EXPECT_DEATH(
        {
            test::RunInCooperator([](coop::Context* ctx)
            {
                StashLocalAddress();

                for (int i = 0; i < 4; i++)
                {
                    ctx->Yield(true);
                }

                volatile int observed = *g_escaped;
                (void)observed;
            });
        },
        "stack-use-after-return");

    if (previous)
    {
        setenv("ASAN_OPTIONS", restore.c_str(), 1);
    }
    else
    {
        unsetenv("ASAN_OPTIONS");
    }
}

#endif // COOP_HAVE_ASAN
