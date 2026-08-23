// Tests for seeded scheduling and the adversarial yield policy
// (CooperatorConfiguration::schedulingMode / yieldPolicy).
//
// A cooperative runtime makes shared state observable only at suspension points, so the set of
// interleavings a program can ever exhibit is decided entirely by which context the scheduler picks
// at each of them. The default policy is a FIFO pop, i.e. strict round-robin, which means that set
// has exactly one member -- the same rotation on every run. A defect that needs a different rotation
// is not rare under round-robin, it is unreachable.
//
// These tests pin the three properties that make the mode worth having: the default order is
// genuinely untouched when the mode is off (and when it is on but round-robin), a fixed seed replays
// a byte-identical schedule, and the adversarial policy reaches interleavings round-robin cannot --
// both the relative order of two observers inside one publisher's window, and the wake handoff,
// which under the default policy passes control from a releaser straight to its waiter with no
// third context ever looking at the state in between.
//

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/cooperator.hpp"
#include "coop/coordinator.h"
#include "coop/signal.h"
#include "coop/thread.h"

namespace
{

using coop::SchedulingMode;
using coop::YieldPolicy;

// COOP_SCHED_SEED / COOP_SCHED_POLICY deliberately override configuration -- that is what lets an
// already-built binary be driven adversarially without a rebuild. It also means these tests, which
// work by configuring a cooperator and asserting on the order it produces, are asserting something
// the environment has taken away from them: under a forced policy every cooperator here gets that
// policy no matter what its configuration says.
//
// Skipping is the honest response. The alternative -- letting configuration win inside the test
// binary -- would carve an exception into the override that the whole replay story depends on, so
// that the tests can keep passing while testing something the shipped mode does not do.
//
bool EnvironmentForcesScheduling()
{
    char const* seed = getenv("COOP_SCHED_SEED");
    char const* policy = getenv("COOP_SCHED_POLICY");
    return (seed && seed[0] != '\0') || (policy && policy[0] != '\0');
}

#define SKIP_IF_ENVIRONMENT_FORCES_SCHEDULING()                                                    \
    do {                                                                                           \
        if (EnvironmentForcesScheduling())                                                         \
        {                                                                                          \
            GTEST_SKIP() << "COOP_SCHED_SEED / COOP_SCHED_POLICY override configuration, so this "  \
                            "test cannot control the policy it asserts on";                        \
        }                                                                                          \
    } while (0)

// Run fn as the root context of a cooperator configured with the given scheduling mode.
//
void RunScheduled(SchedulingMode mode,
                  YieldPolicy policy,
                  uint64_t seed,
                  std::function<void(coop::Context*)> fn)
{
    coop::CooperatorConfiguration cfg = coop::s_defaultCooperatorConfiguration;
    cfg.schedulingMode = mode;
    cfg.yieldPolicy = policy;
    cfg.schedulingSeed = seed;

    coop::Cooperator co(cfg);
    coop::Thread t(&co);

    co.SubmitSync([&](coop::Context* ctx) { fn(ctx); });
    co.Shutdown();
}

// Spawn `workers` contexts that do nothing but take turns, each appending its own digit to a shared
// trace on every turn. The trace *is* the schedule: comparing two of them compares two schedules
// directly, with no instrumentation inside the scheduler.
//
// Recording starts behind a warm-up and a barrier. The warm-up lets the root finish spawning and
// park on the completion signal, so the trace records the workers rotating rather than the root
// interleaved among them. The barrier then aligns the workers, which they need because spawning is
// sequential: the first worker spawned has already taken more turns than the last by the time the
// last one exists, so without it the trace opens on a ragged partial round.
//
std::string ScheduleTrace(SchedulingMode mode,
                          YieldPolicy policy,
                          uint64_t seed,
                          int workers,
                          int turns)
{
    constexpr int kWarmup = 32;

    std::string trace;
    int done = 0;
    int aligned = 0;

    RunScheduled(mode, policy, seed, [&](coop::Context* ctx)
    {
        coop::Signal finished(ctx);

        for (int w = 0; w < workers; ++w)
        {
            coop::Spawn([&, w](coop::Context* c)
            {
                for (int i = 0; i < kWarmup; ++i)
                {
                    c->Yield(true /* force */);
                }

                ++aligned;
                while (aligned < workers)
                {
                    c->Yield(true /* force */);
                }

                for (int i = 0; i < turns; ++i)
                {
                    trace.push_back(static_cast<char>('0' + w));
                    c->Yield(true /* force */);
                }

                if (++done == workers)
                {
                    finished.Notify(c, false /* schedule */);
                }
            });
        }

        finished.Wait(ctx);
    });

    return trace;
}

// One publisher context leaves a two-step mutation half-applied across a yield; two observer
// contexts record, for each window, the order in which they looked at it. Returns the set of
// distinct observer orderings seen across all windows: "12" if observer 1 always got there first,
// "21" for the reverse, and both when the schedule reaches both.
//
std::set<std::string> ObserverOrderings(SchedulingMode mode, YieldPolicy policy, uint64_t seed)
{
    constexpr int kRounds = 400;

    // Keyed by the publisher's step counter, which uniquely identifies the open window.
    //
    std::map<int, std::string> windows;
    int a = 0;
    int b = 0;
    bool stop = false;
    int observersDone = 0;

    RunScheduled(mode, policy, seed, [&](coop::Context* ctx)
    {
        coop::Signal finished(ctx);

        for (int k = 1; k <= 2; ++k)
        {
            coop::Spawn([&, k](coop::Context* c)
            {
                while (!stop)
                {
                    if (a != b)
                    {
                        windows[a].push_back(static_cast<char>('0' + k));
                    }
                    c->Yield(true /* force */);
                }

                if (++observersDone == 2)
                {
                    finished.Notify(c, false /* schedule */);
                }
            });
        }

        coop::Spawn([&](coop::Context* c)
        {
            for (int r = 0; r < kRounds; ++r)
            {
                ++a;
                c->Yield(true /* force */);
                ++b;
                c->Yield(true /* force */);
            }
            stop = true;
        });

        finished.Wait(ctx);
    });

    std::set<std::string> orderings;
    for (auto const& [window, seen] : windows)
    {
        const size_t first1 = seen.find('1');
        const size_t first2 = seen.find('2');
        if (first1 == std::string::npos || first2 == std::string::npos)
        {
            continue;
        }
        orderings.insert(first1 < first2 ? "12" : "21");
    }

    return orderings;
}

// Trace of the wake handoff. The root holds a coordinator per round; a waiter context blocks on
// each, and a set of observer contexts spin. Per round the root records 'P', releases that round's
// coordinator, and records 'p'. 'W' is recorded by the waiter when it resumes, 'O' by an observer
// on each of its turns.
//
// So the character immediately after a 'P' says which policy is in force -- 'W' when the release
// handed control straight to the waiter, 'p' when the releaser kept running -- and any 'O' between
// a 'P' and the following 'W' is an observer that got to look at state the handoff would have
// hidden.
//
std::string HandoffTrace(SchedulingMode mode, YieldPolicy policy, uint64_t seed)
{
    constexpr int kRounds = 8;
    constexpr int kObservers = 3;
    constexpr int kSettleYields = 32;

    std::string trace;

    RunScheduled(mode, policy, seed, [&](coop::Context* ctx)
    {
        coop::Coordinator gates[kRounds];
        bool stop = false;
        int waitersDone = 0;
        int observersDone = 0;

        for (auto& gate : gates)
        {
            ASSERT_TRUE(gate.TryAcquire(ctx));
        }

        for (int r = 0; r < kRounds; ++r)
        {
            coop::Spawn([&, r](coop::Context* c)
            {
                gates[r].Acquire(c);
                trace.push_back('W');
                gates[r].Release(c, false /* schedule */);
                ++waitersDone;
            });
        }

        for (int k = 0; k < kObservers; ++k)
        {
            coop::Spawn([&](coop::Context* c)
            {
                while (!stop)
                {
                    trace.push_back('O');
                    c->Yield(true /* force */);
                }
                ++observersDone;
            });
        }

        for (int i = 0; i < kSettleYields; ++i)
        {
            ctx->Yield(true /* force */);
        }

        for (int r = 0; r < kRounds; ++r)
        {
            trace.push_back('P');
            gates[r].Release(ctx);
            trace.push_back('p');

            for (int i = 0; i < kSettleYields; ++i)
            {
                ctx->Yield(true /* force */);
            }
        }

        stop = true;

        while (waitersDone < kRounds || observersDone < kObservers)
        {
            ctx->Yield(true /* force */);
        }
    });

    return trace;
}

// Count observers that ran between each release point and the waiter it woke.
//
int ObserversInsideHandoffWindow(std::string const& trace)
{
    int inside = 0;
    for (size_t i = 0; i < trace.size(); ++i)
    {
        if (trace[i] != 'P')
        {
            continue;
        }

        for (size_t j = i + 1; j < trace.size() && trace[j] != 'W'; ++j)
        {
            if (trace[j] == 'O')
            {
                ++inside;
            }
        }
    }
    return inside;
}

// The character each release point is followed by, one entry per round.
//
std::string AfterReleasePoints(std::string const& trace)
{
    std::string after;
    for (size_t i = 0; i + 1 < trace.size(); ++i)
    {
        if (trace[i] == 'P')
        {
            after.push_back(trace[i + 1]);
        }
    }
    return after;
}

} // namespace

// The default policy is strict round-robin: the runnable list is a FIFO, a suspending context goes
// to the tail, and the head is resumed. So with N contexts taking turns the trace has period N.
// This is the baseline every other test here is measured against.
//
TEST(SchedulingTest, DefaultModeIsStrictRoundRobin)
{
    SKIP_IF_ENVIRONMENT_FORCES_SCHEDULING();

    constexpr int kWorkers = 3;
    constexpr int kTurns = 64;

    const std::string trace =
        ScheduleTrace(SchedulingMode::Default, YieldPolicy::Fifo, 0, kWorkers, kTurns);

    ASSERT_EQ(trace.size(), static_cast<size_t>(kWorkers * kTurns));

    const std::set<char> firstRound(trace.begin(), trace.begin() + kWorkers);
    EXPECT_EQ(firstRound.size(), static_cast<size_t>(kWorkers));

    for (size_t i = 0; i + kWorkers < static_cast<size_t>(kWorkers * (kTurns - 1)); ++i)
    {
        ASSERT_EQ(trace[i], trace[i + kWorkers]) << "rotation broke at index " << i;
    }
}

// Turning the mode on without turning the policy on changes nothing about the order. This is what
// separates "the mode perturbs the schedule" from "the policy perturbs the schedule": the seed is
// resolved, the selection path is live, and the resulting schedule is still byte-identical to the
// one the default path produces.
//
TEST(SchedulingTest, SeededFifoMatchesDefaultOrder)
{
    SKIP_IF_ENVIRONMENT_FORCES_SCHEDULING();

    constexpr int kWorkers = 3;
    constexpr int kTurns = 64;

    const std::string baseline =
        ScheduleTrace(SchedulingMode::Default, YieldPolicy::Fifo, 0, kWorkers, kTurns);
    const std::string seeded =
        ScheduleTrace(SchedulingMode::Seeded, YieldPolicy::Fifo, 0xC0FFEE, kWorkers, kTurns);

    EXPECT_EQ(baseline, seeded);
}

// The point of the seed: a schedule found once can be run again. Two cooperators given the same
// seed produce the same interleaving down to the last turn, and a different seed produces a
// different one -- otherwise the seed would not be what is driving selection.
//
TEST(SchedulingTest, SameSeedReplaysSameSchedule)
{
    SKIP_IF_ENVIRONMENT_FORCES_SCHEDULING();

    constexpr int kWorkers = 4;
    constexpr int kTurns = 128;

    const std::string first =
        ScheduleTrace(SchedulingMode::Seeded, YieldPolicy::Adversarial, 20260823, kWorkers, kTurns);
    const std::string again =
        ScheduleTrace(SchedulingMode::Seeded, YieldPolicy::Adversarial, 20260823, kWorkers, kTurns);
    const std::string other =
        ScheduleTrace(SchedulingMode::Seeded, YieldPolicy::Adversarial, 7, kWorkers, kTurns);

    EXPECT_EQ(first, again);
    EXPECT_NE(first, other);

    // Adversarial selection must still be a schedule, not a starvation policy: every worker gets
    // all of its turns.
    //
    EXPECT_EQ(first.size(), static_cast<size_t>(kWorkers * kTurns));
}

// Two observers looking at one publisher's half-applied window. Round-robin runs them in a fixed
// rotation, so one of them is always first and the reverse order never happens on any run, at any
// seed, for any number of rounds. The adversarial policy reaches both.
//
// This is the interleaving-reachability claim in its smallest form: round-robin does not make the
// other ordering rare, it makes it impossible, which is indistinguishable from a defect that is not
// there.
//
TEST(SchedulingTest, AdversarialReachesBothObserverOrderings)
{
    SKIP_IF_ENVIRONMENT_FORCES_SCHEDULING();

    const std::set<std::string> roundRobin =
        ObserverOrderings(SchedulingMode::Default, YieldPolicy::Fifo, 0);
    const std::set<std::string> adversarial =
        ObserverOrderings(SchedulingMode::Seeded, YieldPolicy::Adversarial, 20260823);

    EXPECT_EQ(roundRobin.size(), 1u);

    EXPECT_EQ(adversarial.size(), 2u);
    EXPECT_TRUE(adversarial.count("12") == 1);
    EXPECT_TRUE(adversarial.count("21") == 1);
}

// The wake handoff is the runtime's largest scheduling blind spot: a Release with schedule set
// switches from the releaser straight into its waiter, so the interval between them -- exactly where
// a mutation guarded by that coordinator is half-applied -- is never seen by any other runnable
// context. Under the default policy no observer is inside that window on any round; under the
// adversarial policy the releaser keeps running, the wake goes through the runnable queue, and
// observers land in it.
//
TEST(SchedulingTest, AdversarialExposesTheWakeHandoffWindow)
{
    SKIP_IF_ENVIRONMENT_FORCES_SCHEDULING();

    const std::string roundRobin =
        HandoffTrace(SchedulingMode::Default, YieldPolicy::Fifo, 0);
    const std::string adversarial =
        HandoffTrace(SchedulingMode::Seeded, YieldPolicy::Adversarial, 20260823);

    const std::string afterDefault = AfterReleasePoints(roundRobin);
    const std::string afterAdversarial = AfterReleasePoints(adversarial);

    ASSERT_FALSE(afterDefault.empty());
    EXPECT_EQ(afterDefault, std::string(afterDefault.size(), 'W'));

    ASSERT_FALSE(afterAdversarial.empty());
    EXPECT_EQ(afterAdversarial, std::string(afterAdversarial.size(), 'p'));

    EXPECT_EQ(ObserversInsideHandoffWindow(roundRobin), 0);
    EXPECT_GT(ObserversInsideHandoffWindow(adversarial), 0);
}
