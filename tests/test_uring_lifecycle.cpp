#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <sys/resource.h>

#include <gtest/gtest.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/thread.h"

namespace
{

struct RlimitGuard
{
    explicit RlimitGuard(rlim_t softLimit)
    {
        if (getrlimit(RLIMIT_NOFILE, &original) != 0)
        {
            return;
        }

        active = true;
        rlim_t target = softLimit;
        if (original.rlim_max != RLIM_INFINITY && original.rlim_max < target)
        {
            target = original.rlim_max;
        }
        if (original.rlim_cur < target)
        {
            target = original.rlim_cur;
        }

        current = original;
        current.rlim_cur = target;
        if (setrlimit(RLIMIT_NOFILE, &current) != 0)
        {
            active = false;
            current = original;
        }
    }

    ~RlimitGuard()
    {
        if (active)
        {
            (void)setrlimit(RLIMIT_NOFILE, &original);
        }
    }

    rlim_t SoftLimit() const { return current.rlim_cur; }

    struct rlimit original{};
    struct rlimit current{};
    bool active{false};
};

int CountOpenFds()
{
    DIR* dir = opendir("/proc/self/fd");
    if (!dir)
    {
        return -errno;
    }

    int count = 0;
    while (dirent* entry = readdir(dir))
    {
        if (entry->d_name[0] == '.'
            && (entry->d_name[1] == '\0'
                || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
        {
            continue;
        }
        count++;
    }
    closedir(dir);
    return count;
}

void RunOneCooperator()
{
    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);

    ASSERT_TRUE(cooperator.SubmitSync([](coop::Context* ctx)
    {
        ctx->GetCooperator()->Shutdown();
    }));
}

} // end anonymous namespace

TEST(UringLifecycleTest, SequentialCooperatorsDoNotLeakFdsUnderLowLimit)
{
    constexpr rlim_t kSoftFdLimit = 256;
    constexpr int kLifecycles = 320;
    constexpr int kFdSlack = 2;

    RlimitGuard limit(kSoftFdLimit);
    ASSERT_TRUE(limit.active) << "could not lower RLIMIT_NOFILE";

    int startingFds = CountOpenFds();
    ASSERT_GE(startingFds, 0);
    if (static_cast<rlim_t>(startingFds + 64) >= limit.SoftLimit())
    {
        GTEST_SKIP() << "not enough fd headroom under lowered RLIMIT_NOFILE";
    }

    RunOneCooperator();
    int before = CountOpenFds();
    ASSERT_GE(before, 0);

    for (int i = 0; i < kLifecycles; ++i)
    {
        RunOneCooperator();
    }

    int after = CountOpenFds();
    ASSERT_GE(after, 0);

    EXPECT_LE(after, before + kFdSlack);
    EXPECT_GE(after, before - kFdSlack);
}

#ifndef NDEBUG
TEST(UringLifecycleTest, FatalEnterErrorAbortsProcess)
{
    EXPECT_DEATH({
        coop::io::Uring::SetInjectedEnterError(-EBADF);
        coop::Cooperator cooperator;
        coop::Thread thread(&cooperator);
        (void)cooperator.SubmitSync([](coop::Context* ctx) {
            ctx->GetCooperator()->Shutdown();
        });
    }, "Fatal io_uring enter error|uring enter fatal error");
}

TEST(UringLifecycleTest, RetryableEnterErrorDoesNotAbort)
{
    coop::io::Uring::SetInjectedEnterError(-EINTR);
    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);
    ASSERT_TRUE(cooperator.SubmitSync([](coop::Context* ctx) {
        ctx->GetCooperator()->Shutdown();
    }));
}

TEST(UringLifecycleTest, UninitializedUringTeardownFiresWithoutIncrementingCount)
{
    coop::io::Uring::ResetOffOwnerThreadTeardownCount();
    {
        coop::io::Uring uring;
        // uring is not Init()'d, m_initialized == false
    } // ~Uring runs on uninitialized uring path
    EXPECT_EQ(coop::io::Uring::OffOwnerThreadTeardownCount(), 0u);
}

TEST(UringLifecycleTest, OffOwnerThreadTeardownIncrementsCounter)
{
    coop::io::Uring::ResetOffOwnerThreadTeardownCount();
    {
        coop::Cooperator cooperator;
        coop::Thread thread(&cooperator);
        (void)cooperator.SubmitSync([](coop::Context* ctx) {
            ctx->GetCooperator()->Shutdown();
        });
        // thread joins on destruction, then ~Cooperator / ~Uring runs on main thread (off owner thread)
    }
    // S3a: prior to S3b fix, ~Uring runs off-owner-thread on the joining thread with live registration
    EXPECT_GT(coop::io::Uring::OffOwnerThreadTeardownCount(), 0u);
}
#endif
