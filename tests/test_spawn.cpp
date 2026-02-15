#include <gtest/gtest.h>

#include "coop/cooperator.h"
#include "coop/context.h"
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
        auto* obj = ctx->GetCooperator()->Launch<TestLaunchable>(&launched);
        EXPECT_NE(obj, nullptr);
        EXPECT_TRUE(launched);
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
