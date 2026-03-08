#include <gtest/gtest.h>
#include <cstdint>

#include "coop/cooperator_var.hpp"
#include "test_helpers.h"

namespace
{

// Test types for CooperatorVar.
//
struct SimpleState
{
    int value{42};
    uint64_t counter{0};
};

struct WithInit
{
    int phase{0};
    const char* label{nullptr};

    void Init(int p, const char* l)
    {
        phase = p;
        label = l;
    }
};

// These are file-scope static CooperatorVars, registered at static init time.
//
static coop::CooperatorVar<SimpleState> s_simple;
static coop::CooperatorVar<WithInit> s_withInit;

// ---- Registration tests -----------------------------------------------------

TEST(CooperatorVarTest, RegistryHasEntries)
{
    auto& reg = coop::detail::CooperatorVarRegistry::Instance();
    EXPECT_GE(reg.Count(), 2u);
}

TEST(CooperatorVarTest, TotalSizeFitsInBudget)
{
    auto& reg = coop::detail::CooperatorVarRegistry::Instance();
    EXPECT_LE(reg.TotalSize(), coop::Cooperator::LOCAL_STORAGE_SIZE);
}

// ---- Access tests (require a live cooperator) -------------------------------

TEST(CooperatorVarTest, DefaultConstruction)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // SimpleState should have its default values from construction.
        //
        EXPECT_EQ(s_simple->value, 42);
        EXPECT_EQ(s_simple->counter, 0u);
    });
}

TEST(CooperatorVarTest, MutateAndRead)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        s_simple->value = 100;
        s_simple->counter = 999;
        EXPECT_EQ(s_simple->value, 100);
        EXPECT_EQ(s_simple->counter, 999u);
    });
}

TEST(CooperatorVarTest, ExplicitCooperatorAccess)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto* coop = ctx->GetCooperator();
        auto* state = s_simple.Get(coop);
        state->value = 77;
        EXPECT_EQ(s_simple->value, 77);
    });
}

TEST(CooperatorVarTest, PostConstructionInit)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Default construction: phase=0, label=nullptr.
        //
        EXPECT_EQ(s_withInit->phase, 0);
        EXPECT_EQ(s_withInit->label, nullptr);

        // Explicit Init after bootstrap.
        //
        s_withInit->Init(3, "subsystem");
        EXPECT_EQ(s_withInit->phase, 3);
        EXPECT_STREQ(s_withInit->label, "subsystem");
    });
}

TEST(CooperatorVarTest, SharedAcrossContexts)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        s_simple->counter = 0;

        coop::Coordinator done;
        done.TryAcquire(ctx);

        // Spawn a child that increments the counter.
        //
        ctx->GetCooperator()->Spawn([&](coop::Context* child)
        {
            s_simple->counter += 10;
            done.Release(child, false);
        });

        done.Acquire(ctx);
        done.Release(ctx, false);

        // Both contexts see the same CooperatorVar storage.
        //
        EXPECT_EQ(s_simple->counter, 10u);
    });
}

TEST(CooperatorVarTest, IndependentAcrossCooperators)
{
    // Two cooperators should each have their own CooperatorVar storage.
    //
    int value1 = -1;
    int value2 = -1;

    {
        coop::Cooperator coop1;
        coop::Thread t1(&coop1);
        std::binary_semaphore done1(0);

        coop1.Submit([&](coop::Context* ctx)
        {
            s_simple->value = 111;
            value1 = s_simple->value;
            done1.release();
            ctx->GetCooperator()->Shutdown();
        });
        done1.acquire();
    }

    {
        coop::Cooperator coop2;
        coop::Thread t2(&coop2);
        std::binary_semaphore done2(0);

        coop2.Submit([&](coop::Context* ctx)
        {
            // This cooperator should have its own default-constructed state.
            //
            value2 = s_simple->value;
            done2.release();
            ctx->GetCooperator()->Shutdown();
        });
        done2.acquire();
    }

    EXPECT_EQ(value1, 111);
    EXPECT_EQ(value2, 42);  // fresh default, not 111
}

} // namespace
