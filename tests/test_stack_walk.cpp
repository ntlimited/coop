#include <gtest/gtest.h>

#include <csignal>
#include <sys/mman.h>
#include <unistd.h>

#include "coop/detail/stack_walk.h"
#include "coop/perf/sampler.h"

namespace
{

class StackWalkTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        m_mapping = mmap(nullptr, 3 * m_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ASSERT_NE(m_mapping, MAP_FAILED);
        auto* stack = static_cast<char*>(m_mapping) + m_page;
        ASSERT_EQ(mprotect(stack, m_page, PROT_READ | PROT_WRITE), 0);
        m_bounds = {reinterpret_cast<uintptr_t>(stack),
                    reinterpret_cast<uintptr_t>(stack + m_page)};
    }

    void TearDown() override
    {
        if (m_mapping != MAP_FAILED) munmap(m_mapping, 3 * m_page);
    }

    uintptr_t Frame(size_t offset, uintptr_t next, uintptr_t pc)
    {
        auto* frame = reinterpret_cast<uintptr_t*>(m_bounds.lo + offset);
        frame[0] = next;
        frame[1] = pc;
        return reinterpret_cast<uintptr_t>(frame);
    }

    void* m_mapping{MAP_FAILED};
    size_t m_page{0};
    coop::detail::StackBounds m_bounds{0, 0};
    uintptr_t m_frames[8]{};
};

TEST_F(StackWalkTest, RequiresWholeFrameBeforeGuardPage)
{
    // The last aligned word is readable, but its return-address word is in PROT_NONE.
    auto fp = m_bounds.hi - sizeof(uintptr_t);
    EXPECT_EQ(coop::detail::WalkFrameChain(fp, m_bounds, m_frames, 8), 0);
    EXPECT_EQ(coop::detail::WalkFrameChain(m_bounds.hi, m_bounds, m_frames, 8), 0);
    EXPECT_EQ(coop::detail::WalkFrameChain(m_bounds.lo - sizeof(uintptr_t),
                                          m_bounds, m_frames, 8), 0);
    EXPECT_EQ(coop::detail::WalkFrameChain(UINTPTR_MAX - 7, m_bounds, m_frames, 8), 0);
}

TEST_F(StackWalkTest, IncludesCompleteLastFrame)
{
    auto fp = Frame(m_page - 2 * sizeof(uintptr_t), 0, 0x1234);
    EXPECT_EQ(coop::detail::WalkFrameChain(fp, m_bounds, m_frames, 8), 1);
    EXPECT_EQ(m_frames[0], 0x1234);
}

TEST_F(StackWalkTest, RejectsUnalignedAndNonIncreasingLinks)
{
    auto first = Frame(0, m_bounds.lo + 32, 0x1111);
    auto second = Frame(32, first, 0x2222); // cycle
    EXPECT_EQ(coop::detail::WalkFrameChain(first, m_bounds, m_frames, 8), 2);
    EXPECT_EQ(m_frames[0], 0x1111);
    EXPECT_EQ(m_frames[1], 0x2222);
    Frame(32, second, 0x2222); // self-link
    EXPECT_EQ(coop::detail::WalkFrameChain(second, m_bounds, m_frames, 8), 1);
    EXPECT_EQ(coop::detail::WalkFrameChain(first + 1, m_bounds, m_frames, 8), 0);
    Frame(0, second + 1, 0x1111);
    EXPECT_EQ(coop::detail::WalkFrameChain(first, m_bounds, m_frames, 8), 1);
}

TEST_F(StackWalkTest, StopsAtZeroReturnAndOutputLimit)
{
    auto first = Frame(0, m_bounds.lo + 32, 0x1111);
    Frame(32, 0, 0);
    EXPECT_EQ(coop::detail::WalkFrameChain(first, m_bounds, m_frames, 8), 1);
    Frame(32, 0, 0x2222);
    m_frames[1] = 0x3333;
    EXPECT_EQ(coop::detail::WalkFrameChain(first, m_bounds, m_frames, 1), 1);
    EXPECT_EQ(m_frames[1], 0x3333);
    EXPECT_EQ(coop::detail::WalkFrameChain(first, m_bounds, nullptr, 0), 0);
}

TEST_F(StackWalkTest, ChecksCompleteSavedRegisterRecord)
{
    uintptr_t pc = 0, fp = 0;
#if defined(__x86_64__)
    constexpr size_t words = 7, fpSlot = 5, pcSlot = 6;
#elif defined(__aarch64__)
    constexpr size_t words = 12, fpSlot = 0, pcSlot = 1;
#endif
    auto address = m_bounds.hi - words * sizeof(uintptr_t);
    auto* saved = reinterpret_cast<uintptr_t*>(address);
    saved[fpSlot] = 0x1234;
    saved[pcSlot] = 0x5678;
    EXPECT_TRUE(coop::detail::ReadSavedStack(address, m_bounds, pc, fp));
    EXPECT_EQ(pc, 0x5678);
    EXPECT_EQ(fp, 0x1234);
    EXPECT_FALSE(coop::detail::ReadSavedStack(address + sizeof(uintptr_t), m_bounds, pc, fp));
    EXPECT_FALSE(coop::detail::ReadSavedStack(m_bounds.hi, m_bounds, pc, fp));
    EXPECT_FALSE(coop::detail::ReadSavedStack(address + 1, m_bounds, pc, fp));
    EXPECT_FALSE(coop::detail::ReadSavedStack(0, m_bounds, pc, fp));
}

TEST_F(StackWalkTest, InterruptedWalkRequiresMatchingStack)
{
    auto fp = Frame(32, 0, 0x2222);
    EXPECT_EQ(coop::detail::WalkInterruptedStack(0x1111, fp, m_bounds.lo,
                                               m_bounds, m_frames, 8), 2);
    EXPECT_EQ(m_frames[0], 0x1111);
    EXPECT_EQ(m_frames[1], 0x2222);
    // A switch may have changed SP before Scheduled() is updated. Do not follow the FP even
    // if it happens to point inside the old context's segment.
    EXPECT_EQ(coop::detail::WalkInterruptedStack(0x1111, fp, m_bounds.hi,
                                               m_bounds, m_frames, 8), 1);
    EXPECT_EQ(coop::detail::WalkInterruptedStack(0x1111, m_bounds.hi, m_bounds.lo,
                                               {0, 0}, m_frames, 8), 1);
    // On the right stack, reject frames below the interrupted SP; FP == SP is valid.
    EXPECT_EQ(coop::detail::WalkInterruptedStack(0x1111, fp, fp + 16,
                                               m_bounds, m_frames, 8), 1);
    EXPECT_EQ(coop::detail::WalkInterruptedStack(0x1111, fp, fp,
                                               m_bounds, m_frames, 8), 2);
}

TEST(StackSamplingTest, NativeThreadWithoutKnownBoundsCapturesOnlyPc)
{
    coop::perf::ResetSamples();
    coop::perf::SetStackSubsample(1);
    ASSERT_TRUE(coop::perf::StartSampling(2, true));
    raise(SIGPROF);
    coop::perf::StopSampling();
    coop::perf::StackSample sample{};
    ASSERT_EQ(coop::perf::ReadStackSamples(&sample, 1), 1);
    EXPECT_EQ(sample.context, nullptr);
    EXPECT_EQ(sample.depth, 1);
    EXPECT_NE(sample.frames[0], 0);
    coop::perf::SetStackSubsample(10);
}

} // namespace
