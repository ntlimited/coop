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
