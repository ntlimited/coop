#include <gtest/gtest.h>
#include <semaphore>

#include "coop/coordinator.h"
#include "coop/epoch/epoch.h"
#include "coop/self.h"
#include "test_helpers.h"

namespace
{

using Epoch = coop::epoch::Epoch;

// Intrusive retire entry that records when it has been reclaimed.
//
struct TestEntry : coop::epoch::RetireEntry
{
    bool* reclaimed;

    static void Reclaim(coop::epoch::RetireEntry* e)
    {
        *static_cast<TestEntry*>(e)->reclaimed = true;
    }

    void Init(bool* flag)
    {
        reclaimed = flag;
        reclaim   = &TestEntry::Reclaim;
    }
};

// Run a test inside a cooperator. The epoch::Manager is provided automatically by the
// Cooperator — no bootstrap boilerplate required.
//
inline void RunWithEpoch(std::function<void(coop::Context*, coop::epoch::Manager&)> fn)
{
    test::RunInCooperator([fn = std::move(fn)](coop::Context* ctx)
    {
        fn(ctx, *coop::epoch::GetManager());
    });
}

} // end anon namespace

// ---- Epoch counter ----

TEST(EpochTest, InitialEpoch)
{
    RunWithEpoch([](coop::Context*, coop::epoch::Manager& mgr)
    {
        EXPECT_EQ(mgr.Current(), Epoch{1});
    });
}

TEST(EpochTest, Advance)
{
    RunWithEpoch([](coop::Context*, coop::epoch::Manager& mgr)
    {
        EXPECT_EQ(mgr.Advance(), Epoch{2});
        EXPECT_EQ(mgr.Advance(), Epoch{3});
        EXPECT_EQ(mgr.Current(), Epoch{3});
    });
}

// ---- Traversal pins ----

TEST(EpochTest, SafeEpochNoPins)
{
    RunWithEpoch([](coop::Context*, coop::epoch::Manager& mgr)
    {
        EXPECT_EQ(mgr.SafeEpoch(), Epoch::Alive());
    });
}

TEST(EpochTest, TraversalPinSetsSafeEpoch)
{
    RunWithEpoch([](coop::Context* ctx, coop::epoch::Manager& mgr)
    {
        auto ep = mgr.Enter(ctx);
        EXPECT_EQ(ep, Epoch{1});
        EXPECT_EQ(mgr.SafeEpoch(), Epoch{1});

        mgr.Exit(ctx);
        EXPECT_EQ(mgr.SafeEpoch(), Epoch::Alive());
    });
}

TEST(EpochTest, TraversalPinAfterAdvance)
{
    RunWithEpoch([](coop::Context* ctx, coop::epoch::Manager& mgr)
    {
        mgr.Advance();  // epoch = 2

        auto ep = mgr.Enter(ctx);
        EXPECT_EQ(ep, Epoch{2});
        EXPECT_EQ(mgr.SafeEpoch(), Epoch{2});

        mgr.Exit(ctx);
        EXPECT_EQ(mgr.SafeEpoch(), Epoch::Alive());
    });
}

TEST(EpochTest, GuardPinsAndUnpins)
{
    RunWithEpoch([](coop::Context* ctx, coop::epoch::Manager& mgr)
    {
        EXPECT_EQ(mgr.SafeEpoch(), Epoch::Alive());
        {
            coop::epoch::Guard guard(mgr, ctx);
            EXPECT_EQ(guard.PinnedEpoch(), Epoch{1});
            EXPECT_EQ(mgr.SafeEpoch(), Epoch{1});
        }
        EXPECT_EQ(mgr.SafeEpoch(), Epoch::Alive());
    });
}

// ---- Application pins ----

TEST(EpochTest, ApplicationPinSetsSafeEpoch)
{
    RunWithEpoch([](coop::Context* ctx, coop::epoch::Manager& mgr)
    {
        mgr.Pin(ctx, Epoch{1});
        EXPECT_EQ(mgr.SafeEpoch(), Epoch{1});

        // Epoch advances but pin stays at 1.
        //
        mgr.Advance();
        EXPECT_EQ(mgr.SafeEpoch(), Epoch{1});

        mgr.Unpin(ctx);
        EXPECT_EQ(mgr.SafeEpoch(), Epoch::Alive());
    });
}

TEST(EpochTest, BothPinsTakenMinimum)
{
    RunWithEpoch([](coop::Context* ctx, coop::epoch::Manager& mgr)
    {
        mgr.Advance();         // epoch = 2
        mgr.Pin(ctx, Epoch{1}); // application at 1
        mgr.Enter(ctx);         // traversal at 2

        EXPECT_EQ(mgr.SafeEpoch(), Epoch{1});  // minimum of 1 and 2

        mgr.Exit(ctx);
        EXPECT_EQ(mgr.SafeEpoch(), Epoch{1});  // only application remains

        mgr.Unpin(ctx);
        EXPECT_EQ(mgr.SafeEpoch(), Epoch::Alive());
    });
}

// ---- Retire and reclaim ----

TEST(EpochTest, ReclaimNoPins)
{
    RunWithEpoch([](coop::Context*, coop::epoch::Manager& mgr)
    {
        bool reclaimed = false;
        TestEntry entry;
        entry.Init(&reclaimed);

        mgr.Retire(&entry);
        EXPECT_EQ(mgr.PendingCount(), 1u);

        EXPECT_EQ(mgr.Reclaim(), 1u);
        EXPECT_EQ(mgr.PendingCount(), 0u);
        EXPECT_TRUE(reclaimed);
    });
}

TEST(EpochTest, ReclaimBlockedByTraversalPin)
{
    RunWithEpoch([](coop::Context* ctx, coop::epoch::Manager& mgr)
    {
        bool reclaimed = false;
        TestEntry entry;
        entry.Init(&reclaimed);

        mgr.Enter(ctx);      // pin at epoch 1
        mgr.Retire(&entry);  // retiredAt = 1

        // retiredAt(1) < safe(1) is false — blocked.
        //
        EXPECT_EQ(mgr.Reclaim(), 0u);
        EXPECT_FALSE(reclaimed);

        mgr.Exit(ctx);
        EXPECT_EQ(mgr.Reclaim(), 1u);
        EXPECT_TRUE(reclaimed);
    });
}

TEST(EpochTest, ReclaimBlockedByApplicationPin)
{
    RunWithEpoch([](coop::Context* ctx, coop::epoch::Manager& mgr)
    {
        bool reclaimed = false;
        TestEntry entry;
        entry.Init(&reclaimed);

        mgr.Pin(ctx, Epoch{1});
        mgr.Retire(&entry);

        EXPECT_EQ(mgr.Reclaim(), 0u);
        EXPECT_FALSE(reclaimed);

        mgr.Unpin(ctx);
        EXPECT_EQ(mgr.Reclaim(), 1u);
        EXPECT_TRUE(reclaimed);
    });
}

TEST(EpochTest, PartialReclaimAtEpochBoundary)
{
    RunWithEpoch([](coop::Context* ctx, coop::epoch::Manager& mgr)
    {
        bool r1 = false, r2 = false, r3 = false;
        TestEntry e1, e2, e3;
        e1.Init(&r1);
        e2.Init(&r2);
        e3.Init(&r3);

        mgr.Retire(&e1);  // retiredAt = 1
        mgr.Advance();    // epoch = 2
        mgr.Retire(&e2);  // retiredAt = 2
        mgr.Advance();    // epoch = 3
        mgr.Retire(&e3);  // retiredAt = 3

        // Pin at epoch 2 — e1 (retiredAt=1) is reclaimable, e2 and e3 are not.
        //
        mgr.Pin(ctx, Epoch{2});
        EXPECT_EQ(mgr.Reclaim(), 1u);
        EXPECT_TRUE(r1);
        EXPECT_FALSE(r2);
        EXPECT_FALSE(r3);
        EXPECT_EQ(mgr.PendingCount(), 2u);

        mgr.Unpin(ctx);
        EXPECT_EQ(mgr.Reclaim(), 2u);
        EXPECT_TRUE(r2);
        EXPECT_TRUE(r3);
        EXPECT_EQ(mgr.PendingCount(), 0u);
    });
}

// ---- Cross-context pin visibility ----

TEST(EpochTest, BlockedContextPinVisibleInSafeEpoch)
{
    RunWithEpoch([](coop::Context* ctx, coop::epoch::Manager& mgr)
    {
        bool reclaimed = false;
        TestEntry entry;
        entry.Init(&reclaimed);

        // Pre-acquire go so the child blocks after pinning, keeping the pin alive
        // while the parent does its reclaim check.
        //
        coop::Coordinator go;
        go.TryAcquire(ctx);

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            mgr.Enter(child);  // pin at epoch 1
            go.Acquire(child); // parent holds go — child blocks here
            mgr.Exit(child);
        });

        // Child has run, pinned, and blocked on go. Parent resumes here.
        //
        mgr.Retire(&entry);  // retiredAt = 1
        mgr.Advance();       // epoch = 2

        EXPECT_EQ(mgr.Reclaim(), 0u);  // child still pinned at 1
        EXPECT_FALSE(reclaimed);

        go.Release(ctx, false /* schedule */);
        ctx->Yield(true);  // let child unpin and exit

        EXPECT_EQ(mgr.Reclaim(), 1u);
        EXPECT_TRUE(reclaimed);
    });
}

// ---- Cross-cooperator pin visibility ----

// Two cooperators, each with its own epoch::Manager. A reader on cooperator B pins at epoch 1
// while the writer on cooperator A tries to reclaim. The writer's SafeEpoch() must see the
// reader's pin via the published watermark and block reclamation until the reader exits.
//
// Coordinators are not safe across cooperators (they manipulate a single cooperator's context
// queues). Cross-thread synchronization uses std::binary_semaphore instead — each cooperator
// has exactly one context in this test so blocking the cooperator thread is fine.
//
TEST(EpochTest, CrossCooperatorPinBlocksReclaim)
{
    bool reclaimed = false;

    // pinned:   B → A: watermark is live
    // reclaim:  A → B: reclaim attempt done, B may unpin now
    // released: B → A: B has unpinned, A may do final reclaim
    //
    std::binary_semaphore pinned{0}, reclaim{0}, released{0};

    // Writer cooperator (A) — owns the retire queue and drives reclamation.
    //
    coop::Cooperator coopA;
    coop::Thread threadA(&coopA);

    // Reader cooperator (B) — will pin at epoch 1.
    //
    coop::Cooperator coopB;
    coop::Thread threadB(&coopB);

    coopB.Submit([&](coop::Context* ctx)
    {
        coop::epoch::Manager mgrB;
        coop::epoch::SetManager(&mgrB);

        mgrB.Enter(ctx);   // pin at epoch 1 — watermark published to coopB->m_epochWatermark
        pinned.release();  // happens-before A's pinned.acquire() → A sees watermark

        reclaim.acquire(); // block until A has attempted reclaim

        mgrB.Exit(ctx);    // unpin — watermark reset to Alive()
        released.release();

        coop::epoch::SetManager(nullptr);
        ctx->GetCooperator()->Shutdown();
    });

    coopA.Submit([&](coop::Context* ctx)
    {
        coop::epoch::Manager mgrA;
        coop::epoch::SetManager(&mgrA);

        TestEntry entry;
        entry.Init(&reclaimed);
        mgrA.Retire(&entry);  // retiredAt = 1

        pinned.acquire();  // wait for B to publish watermark = 1

        // B's watermark = 1. SafeEpoch() across registry = 1.
        // entry.retiredAt(1) < safeEpoch(1) is false — blocked.
        //
        EXPECT_EQ(mgrA.Reclaim(), 0u);
        EXPECT_FALSE(reclaimed);

        reclaim.release(); // let B unpin
        released.acquire();

        // B's watermark = Alive(). SafeEpoch() = Alive(). Now reclaimable.
        //
        EXPECT_EQ(mgrA.Reclaim(), 1u);
        EXPECT_TRUE(reclaimed);

        coop::epoch::SetManager(nullptr);
        ctx->GetCooperator()->Shutdown();
    });
}
