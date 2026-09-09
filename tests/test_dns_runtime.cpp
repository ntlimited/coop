#include <gtest/gtest.h>

#include "coop/group.h"
#include "coop/io/resolve.h"
#include "test_helpers.h"

namespace
{

// Each RunInCooperator owns a fresh default configuration slot. Starting several lookups before
// setup IO completes catches publication-before-read bugs, even on a single scheduler thread.
// These integration tests require io_uring; packet/config tests have a separate kernel-free binary.
//
void FirstUse(coop::Context* ctx)
{
    coop::Group group(ctx);
    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(group.Go([](coop::Context*)
        {
            in_addr result{};
            int r = coop::io::Resolve4("localhost", &result);
            EXPECT_EQ(r, 0);
            EXPECT_EQ(result.s_addr, htonl(INADDR_LOOPBACK));
        }));
    }
    EXPECT_TRUE(group.Wait());
}

TEST(DnsRuntimeTest, ConcurrentFirstUseWaitsForCompleteConfiguration)
{
    test::RunInCooperator(FirstUse);
}

TEST(DnsRuntimeTest, ConfigurationOwnershipIsPerCooperator)
{
    // Construct cooperators serially; only resolver initialization is under test here.
    coop::Cooperator a, b;
    coop::Thread ta(&a), tb(&b);
    auto run = [](coop::Context* ctx)
    {
        FirstUse(ctx);
        ctx->GetCooperator()->Shutdown();
    };
    ASSERT_TRUE(a.Submit(decltype(run)(run)));
    ASSERT_TRUE(b.Submit(decltype(run)(run)));
}

} // namespace
