#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/coordinate_with.h"
#include "coop/debug/introspect.h"

#include "test_helpers.h"

namespace
{

struct UnnamedContext : coop::Context
{
    UnnamedContext()
        : Context(nullptr, coop::s_defaultConfiguration, nullptr, nullptr)
    {
    }
};

} // namespace

TEST(IntrospectTest, UnnamedContextInitializesName)
{
    // StackPool reuses storage without clearing it. Exercise the actual constructor on
    // nonzero storage, without starting a scheduler or relying on allocator contents.
    // This context never runs; skip its scheduler-dependent destructor. Its constructor
    // allocates no external resources and the local backing storage ends its lifetime.
    //
    alignas(UnnamedContext) unsigned char storage[sizeof(UnnamedContext)];
    std::memset(storage, 0xa5, sizeof(storage));
    auto* context = new (storage) UnnamedContext;

    // Compare representations first: on the broken constructor the pointer is still
    // indeterminate, so neither evaluate it nor hand it to a string comparison.
    //
    const char* expected = nullptr;
    ASSERT_EQ(std::memcmp(&context->m_name, &expected, sizeof(expected)), 0);
    EXPECT_STREQ(context->GetName(), "[anonymous]");
}

// A distinctively-named, non-inlined function that parks the calling context on a coordinator. It
// has external linkage so -rdynamic exposes it to dladdr; the test asserts this exact name shows up
// in the *suspended* context's walked stack — proof that coop can recover a blocked context's call
// chain, the thing stackless coroutines cannot do.
//
coop::Coordinator* g_introspectGate = nullptr;

[[gnu::noinline]] void ParkInDistinctiveFrame(coop::Context* c)
{
    coop::CoordinateWith(c, g_introspectGate);
}

TEST(IntrospectTest, WalksBlockedContextStack)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::Coordinator gate;
        gate.TryAcquire(ctx);            // hold it so the child blocks
        g_introspectGate = &gate;

        coop::Context::Handle child;
        ctx->GetCooperator()->Spawn([](coop::Context* c)
        {
            c->SetName("blocker");
            ParkInDistinctiveFrame(c);   // parks here until the gate is released
        }, &child);

        for (int i = 0; i < 5; i++) ctx->Yield(true);   // let the child reach the block

        // Dump every context to an in-memory stream and inspect it.
        //
        char* buf = nullptr;
        size_t sz = 0;
        FILE* f = open_memstream(&buf, &sz);
        coop::debug::DumpContexts(ctx->GetCooperator(), f);
        fclose(f);
        std::string dump(buf, sz);
        free(buf);

        // The blocked child appears by name, marked blocked, with its real call frame recovered.
        //
        EXPECT_NE(dump.find("context 'blocker'"), std::string::npos) << dump;
        EXPECT_NE(dump.find("state=blocked"), std::string::npos) << dump;
        EXPECT_NE(dump.find("ParkInDistinctiveFrame"), std::string::npos) << dump;

        gate.Release(ctx);               // let the child run to completion
        for (int i = 0; i < 5; i++) ctx->Yield(true);
    });
}

// CaptureStack on the running context (the caller itself) returns a live, non-empty frame.
//
TEST(IntrospectTest, WalksRunningContext)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        uintptr_t frames[32];
        int depth = coop::debug::CaptureStack(ctx, frames, 32);
        EXPECT_GE(depth, 1);
        if (depth >= 2) EXPECT_NE(frames[0], frames[1]);
    });
}

// The scheduler heartbeat (Cooperator::Loops, behind /api/health) advances as the loop runs.
//
TEST(IntrospectTest, LoopHeartbeatAdvances)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto* co = ctx->GetCooperator();
        uint64_t before = co->Loops();
        for (int i = 0; i < 32; i++) ctx->Yield(true);   // drive outer-loop passes
        EXPECT_GT(co->Loops(), before);
    });
}
