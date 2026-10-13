#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/context.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"
#include "coop/launchable.h"
#include "coop/self.h"
#include "test_helpers.h"

TEST(SpawnTest, BasicSpawn)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        bool ran = false;
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            ran = true;
        });
        EXPECT_TRUE(ran);
    });
}

TEST(SpawnTest, SpawnYield)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int step = 0;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            EXPECT_EQ(step, 0);
            step = 1;
            child->Yield(true);
            // Resumed
            //
            EXPECT_EQ(step, 2);
            step = 3;
        });

        // Child yielded, we're back
        //
        EXPECT_EQ(step, 1);
        step = 2;
        ctx->Yield(true);

        EXPECT_EQ(step, 3);
    });
}

TEST(SpawnTest, ParentChild)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            EXPECT_EQ(child->Parent(), ctx);
        });
    });
}

TEST(SpawnTest, KillChild)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle handle;
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            EXPECT_FALSE(child->IsKilled());

            // Yield back so parent can kill us
            //
            child->Yield(true);

            // Resumed — should now be killed
            //
            EXPECT_TRUE(child->IsKilled());
        }, &handle);

        // Child yielded, kill it
        //
        handle.Kill();

        // Yield to let child run its killed check
        //
        ctx->Yield(true);
    });
}

namespace
{

struct TestLaunchable : coop::Launchable
{
    TestLaunchable(coop::Context* ctx, bool* flag)
    : coop::Launchable(ctx)
    , m_flag(flag)
    {
    }

    virtual void Launch() final
    {
        *m_flag = true;
    }

    bool* m_flag;
};

} // end anonymous namespace

TEST(SpawnTest, LaunchBasic)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        bool launched = false;
        auto result = ctx->GetCooperator()->Launch<TestLaunchable>(&launched);
        EXPECT_TRUE(result.spawned);
        EXPECT_EQ(result.object, nullptr);
        EXPECT_TRUE(launched);
    });
}

namespace
{

struct YieldingLaunchable : coop::Launchable
{
    YieldingLaunchable(coop::Context* ctx, int* step)
    : coop::Launchable(ctx)
    , m_step(step)
    {
    }

    virtual void Launch() final
    {
        *m_step = 1;
        GetContext()->Yield(true);
        *m_step = 2;
    }

    coop::Context* Ctx() { return GetContext(); }

    int* m_step;
};

} // end anonymous namespace

TEST(SpawnTest, LaunchReturnsLiveObjectIfYields)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int step = 0;
        auto result = ctx->GetCooperator()->Launch<YieldingLaunchable>(&step);
        EXPECT_TRUE(result.spawned);
        ASSERT_NE(result.object, nullptr);
        EXPECT_EQ(step, 1);
        EXPECT_EQ(result.object->Ctx()->Parent(), ctx);

        ctx->Yield(true);
        EXPECT_EQ(step, 2);
    });
}

TEST(SpawnTest, LaunchFailsWhenParentKilled)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle child;
        ctx->GetCooperator()->Spawn([&](coop::Context* c)
        {
            c->Yield(true);
            bool launched = false;
            auto result = c->GetCooperator()->Launch<TestLaunchable>(&launched);
            EXPECT_FALSE(result.spawned);
            EXPECT_EQ(result.object, nullptr);
            EXPECT_FALSE(launched);
        }, &child);

        child.Kill();
        ctx->Yield(true);
    });
}

TEST(SpawnTest, MultipleContexts)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int count = 0;
        for (int i = 0; i < 5; i++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* child)
            {
                count++;
            });
        }
        EXPECT_EQ(count, 5);
    });
}

TEST(SpawnTest, ManyContexts)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int count = 0;
        for (int i = 0; i < 100; i++)
        {
            ctx->GetCooperator()->Spawn([&](coop::Context* child)
            {
                count++;
                child->Yield(true);
                count++;
            });
        }

        // After spawning, all children have run once and yielded. Resume them so they
        // complete their second increment and exit.
        //
        while (count < 200)
        {
            ctx->Yield(true);
        }

        EXPECT_EQ(count, 200);
    });
}

TEST(SpawnTest, DeepNesting)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        constexpr int DEPTH = 10;
        int deepest = 0;
        coop::Context* parents[DEPTH + 1];
        parents[0] = ctx;

        // Build a chain: ctx spawns child[0], which spawns child[1], etc.
        // Each child records its parent and depth, then yields.
        //
        std::function<void(coop::Context*, int)> spawnChain;
        spawnChain = [&](coop::Context* parent, int depth)
        {
            if (depth > DEPTH)
            {
                return;
            }

            parent->GetCooperator()->Spawn([&, depth](coop::Context* child)
            {
                EXPECT_EQ(child->Parent(), parent);
                parents[depth] = child;
                deepest = depth;

                spawnChain(child, depth + 1);

                // Yield to let parent verify
                //
                child->Yield(true);
            });
        };

        spawnChain(ctx, 1);

        EXPECT_EQ(deepest, DEPTH);

        // Verify the chain
        //
        for (int i = 1; i <= DEPTH; i++)
        {
            EXPECT_EQ(parents[i]->Parent(), parents[i - 1]);
        }
    });
}

TEST(SpawnTest, SpawnFromKilledContextFails)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle handle;
        bool spawnResult = true;

        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            // Yield back so parent can kill us
            //
            child->Yield(true);

            // We're now killed — try to spawn
            //
            EXPECT_TRUE(child->IsKilled());
            spawnResult = child->GetCooperator()->Spawn([](coop::Context* grandchild)
            {
                // Should never run
                //
                FAIL() << "Grandchild should not have been spawned from killed context";
            });
        }, &handle);

        handle.Kill();
        ctx->Yield(true);

        EXPECT_FALSE(spawnResult);
    });
}

TEST(SpawnTest, BoundaryCrossingKill)
{
    // Kill a context from outside the cooperator's thread. This exercises the BoundarySafeKill
    // boundary-crossing path: Submit carries the kill into the cooperator thread, which then
    // validates the handle and performs the actual kill.
    //
    struct SharedState
    {
        std::atomic<bool>       childBlocked{false};
        std::atomic<bool>       childKilled{false};
        coop::Context::Handle   handle;
    } state;

    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        auto* state = static_cast<SharedState*>(arg);
        coop::Coordinator coord;
        coord.TryAcquire(ctx);

        ctx->GetCooperator()->Spawn([&coord, state](coop::Context* child)
        {
            // Block kill-aware so the cross-thread kill can wake us
            //
            state->childBlocked.store(true, std::memory_order_release);
            auto result = coop::CoordinateWithKill(child, &coord);

            state->childKilled.store(result.Killed(), std::memory_order_release);
        }, &state->handle);

        // Yield until killed
        //
        while (!ctx->IsKilled())
        {
            ctx->Yield(true);
        }
    }, &state);

    // Wait for the child to be blocked, then kill it from this (non-cooperator) thread
    //
    while (!state.childBlocked.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    state.handle.Kill();

    // Give the cooperator time to process the kill
    //
    for (int i = 0; i < 100 && !state.childKilled.load(std::memory_order_acquire); i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(state.childKilled.load(std::memory_order_acquire));

    cooperator.Shutdown();
}

TEST(SpawnTest, StackPoolReuse)
{
    test::RunInCooperator(
        [](coop::Context* ctx)
        {
            coop::SpawnConfiguration smallConfig = {.priority = 0, .stackSize = 16384};
            coop::Context* firstAddr = nullptr;
            coop::Context* secondAddr = nullptr;

            ctx->GetCooperator()->Spawn(smallConfig,
                                        [&](coop::Context* child) { firstAddr = child; });

            ctx->GetCooperator()->Spawn(smallConfig,
                                        [&](coop::Context* child) { secondAddr = child; });

            EXPECT_NE(firstAddr, nullptr);
            EXPECT_EQ(firstAddr, secondAddr);
        });
}

TEST(SpawnTest, StackPoolReuseDifferentSizes)
{
    test::RunInCooperator(
        [](coop::Context* ctx)
        {
            // Explicit pooled sizes: the configured default may bypass the pool entirely.
            // Spawn with 16KB, then 32KB — different buckets, should NOT reuse
            //
            coop::SpawnConfiguration smallConfig = {.priority = 0, .stackSize = 16384};
            coop::Context* firstAddr = nullptr;
            coop::Context* secondAddr = nullptr;

            ctx->GetCooperator()->Spawn(smallConfig,
                                        [&](coop::Context* child) { firstAddr = child; });

            coop::SpawnConfiguration bigConfig = {.priority = 0, .stackSize = 32768};
            ctx->GetCooperator()->Spawn(bigConfig,
                                        [&](coop::Context* child) { secondAddr = child; });

            EXPECT_NE(firstAddr, nullptr);
            EXPECT_NE(secondAddr, nullptr);
            EXPECT_NE(firstAddr, secondAddr);
        });
}

TEST(SpawnTest, KillParentKillsChildren)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle parentHandle;
        bool childKilled = false;
        bool grandchildKilled = false;

        ctx->GetCooperator()->Spawn([&](coop::Context* parent)
        {
            parent->GetCooperator()->Spawn([&](coop::Context* child)
            {
                child->GetCooperator()->Spawn([&](coop::Context* grandchild)
                {
                    grandchild->Yield(true);
                    grandchildKilled = grandchild->IsKilled();
                });

                child->Yield(true);
                childKilled = child->IsKilled();
            });

            // All children are yielded. Yield back so outer context can kill us.
            //
            parent->Yield(true);
        }, &parentHandle);

        // Kill the parent — children should also be killed
        //
        parentHandle.Kill();

        // Yield to let the killed contexts run their post-kill checks
        //
        for (int i = 0; i < 5; i++)
        {
            ctx->Yield(true);
        }

        EXPECT_TRUE(childKilled);
        EXPECT_TRUE(grandchildKilled);
    });
}

namespace
{

// Red calibration for the launch-window scheduler-accounting bug: a
// Launchable whose CONSTRUCTOR spawns (the ShutdownOnKillGuard pattern —
// coop/io/shutdown_on_kill.cpp). Before the LAUNCHING state, the outer
// context sat in m_contexts as YIELDED without m_yielded membership while
// its constructor ran, and the inner Spawn's EnterContext SanityCheck
// aborted (`yielded == YieldedCount()`, first hit via strata_server's
// /healthz HttpConnection, 2026-08-01).
struct SpawnInCtorLaunchable : coop::Launchable
{
    SpawnInCtorLaunchable(coop::Context* ctx, bool* ctorSpawnRan, bool* launched)
    : coop::Launchable(ctx)
    , m_launched(launched)
    {
        ctx->GetCooperator()->Spawn([ctorSpawnRan](coop::Context*)
        {
            *ctorSpawnRan = true;
        });
    }

    virtual void Launch() final
    {
        *m_launched = true;
    }

    bool* m_launched;
};

} // end anonymous namespace

TEST(SpawnTest, HandleGetKilledSignalNullAfterExit)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Context::Handle handle;
        ctx->GetCooperator()->Spawn([](coop::Context*) {}, &handle);
        ctx->Yield(true);
        EXPECT_FALSE(handle);
        EXPECT_EQ(handle.GetKilledSignal(), nullptr);
        handle.Kill();
    });
}

TEST(SpawnTest, LaunchWhoseConstructorSpawns)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        bool ctorSpawnRan = false;
        bool launched = false;
        auto result = ctx->GetCooperator()->Launch<SpawnInCtorLaunchable>(
            &ctorSpawnRan, &launched);
        EXPECT_TRUE(result.spawned);
        EXPECT_EQ(result.object, nullptr);
        EXPECT_TRUE(launched);
        ctx->Yield(true);
        EXPECT_TRUE(ctorSpawnRan);
    });
}

// Check storage before construction as well as live contexts: an optimizing compiler may
// assume a typed Context* is aligned even when the allocator broke that contract.
//
TEST(SpawnTest, ContextStorageAlignment)
{
    coop::StackPool pool;
    for (size_t size : {size_t(4096), size_t(16384), size_t(131072), size_t(262144)})
    {
        for (int round = 0; round < 64; ++round)
        {
            void* storage = pool.Allocate(size);
            ASSERT_NE(storage, nullptr);
            EXPECT_EQ(reinterpret_cast<uintptr_t>(storage) % alignof(coop::Context), 0u);
            pool.Free(storage, size);
        }
    }
    // Fill past the cache cap to exercise eviction, then drain all retained blocks.
    //
    void* blocks[40];
    for (void*& block : blocks)
    {
        block = pool.Allocate(16384);
        ASSERT_NE(block, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(block) % alignof(coop::Context), 0u);
    }
    for (void* block : blocks)
        pool.Free(block, 16384);
    EXPECT_GT(pool.GetStats().hits, 0u);
    pool.Drain();
    EXPECT_EQ(pool.GetStats().cached, 0u);
}

TEST(SpawnTest, EveryContextAlignedAcrossPoolReuse)
{
    test::RunInCooperator(
        [](coop::Context* ctx)
        {
            EXPECT_EQ(reinterpret_cast<uintptr_t>(ctx) % alignof(coop::Context), 0u);
            for (size_t size : {size_t(4096), size_t(16384), size_t(131072), size_t(262144)})
            {
                for (int round = 0; round < 64; ++round)
                {
                    EXPECT_TRUE(ctx->GetCooperator()->Spawn(
                        {.priority = 0, .stackSize = size},
                        [](coop::Context* child)
                        {
                            EXPECT_EQ(reinterpret_cast<uintptr_t>(child) % alignof(coop::Context),
                                      0u);
                            EXPECT_EQ(reinterpret_cast<uintptr_t>(child->m_segment.Bottom()) % 128,
                                      0u);
                            EXPECT_EQ(reinterpret_cast<uintptr_t>(child->m_segment.Top()) % 16, 0u);
                        }));
                }
            }
        });
}

namespace
{
struct alignas(4096) AlignedSpawn
{
    bool* ran;
    void operator()(coop::Context*)
    {
        volatile uintptr_t address = reinterpret_cast<uintptr_t>(this);
        EXPECT_EQ(address % 4096, 0u);
        *ran = true;
    }
};

struct alignas(4096) AlignedLaunch : coop::Launchable
{
    bool* ran;
    AlignedLaunch(coop::Context* ctx, bool* result) : Launchable(ctx), ran(result) {}
    void Launch() final
    {
        volatile uintptr_t address = reinterpret_cast<uintptr_t>(this);
        EXPECT_EQ(address % 4096, 0u);
        *ran = true;
    }
};
} // namespace

TEST(SpawnTest, OverAlignedStackObjects)
{
    // The caller's page-aligned temporaries need room independently of child stacks.
    //
    test::RunInCooperator(
        [](coop::Context* ctx)
        {
            for (int round = 0; round < 64; ++round)
            {
                bool spawned = false;
                bool launched = false;
                EXPECT_TRUE(ctx->GetCooperator()->Spawn(AlignedSpawn{&spawned}));
                EXPECT_TRUE(ctx->GetCooperator()->Launch<AlignedLaunch>(&launched));
                EXPECT_TRUE(spawned);
                EXPECT_TRUE(launched);
            }
        }, {.stackSize = 65536});
}

// A page-aligned, page-sized object cannot fit after ContextVars on a 4KB stack.
// Reject before moving/constructing it, including NDEBUG, and recycle the context cleanly.
//
TEST(SpawnTest, OverAlignedObjectsRefusedOnTightStack)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto* co = ctx->GetCooperator();
        for (int round = 0; round < 64; ++round)
        {
            bool spawned = false;
            bool launched = false;
            coop::Context::Handle handle;
            EXPECT_FALSE(co->Spawn({.stackSize = 4096}, AlignedSpawn{&spawned}, &handle));
            EXPECT_FALSE(handle);
            EXPECT_FALSE(spawned);
            auto result = co->Launch<AlignedLaunch>(
                {.stackSize = 4096}, &handle, &launched);
            EXPECT_FALSE(result.spawned);
            EXPECT_EQ(result.object, nullptr);
            EXPECT_FALSE(handle);
            EXPECT_FALSE(launched);
            EXPECT_TRUE(ctx->m_children.IsEmpty());
            // A successful ordinary spawn must still work after repeated refusal/pool reuse.
            //
            EXPECT_TRUE(co->Spawn({.stackSize = 4096},
                                 [&spawned](coop::Context*) { spawned = true; }));
            EXPECT_TRUE(spawned);
        }
    }, {.stackSize = 65536});
}
