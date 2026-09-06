#include <csignal>
#include <cstdint>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/signal_stack.h"
#include "test_helpers.h"

// What a signal costs when it lands on a context stack, and what an alternate stack does about it.
//
// The number that makes this worth having is the platform's own: sysconf(_SC_SIGSTKSZ) reports what
// a handler needs, and on a machine with wide vector state that is tens of kilobytes — more than a
// context deep in a call chain has left. A handler taken there runs off the end of the segment. The
// tests below take a real signal at real depth on a real context stack and check where the frame
// landed, in both configurations, because "the flag is set" is a claim about the code and "the frame
// landed elsewhere" is a claim about the machine.

namespace
{

volatile sig_atomic_t g_handlerRan = 0;
volatile sig_atomic_t g_handlerSawOnStack = 0;
void* volatile        g_signalFrame = nullptr;

// Records where the kernel put the signal frame.
//
// The ucontext handed to a SA_SIGINFO handler lives inside that frame, so its address is the frame's
// address, and the frame is the thing whose size is the problem — it is what the kernel writes
// before the handler gets a say. Taking the address of a local instead would measure something else
// entirely: under ASan's fake stacks a handler's locals are not on any real stack at all.
//
// Everything touched here is a volatile global or a syscall, so it stays async-signal-safe. GTest's
// machinery is not, and belongs on the far side of the return.
//
void RecordingHandler(int, siginfo_t*, void* uctx)
{
    g_signalFrame = uctx;

    stack_t current{};
    g_handlerSawOnStack =
        (sigaltstack(nullptr, &current) == 0) && ((current.ss_flags & SS_ONSTACK) != 0);

    g_handlerRan = 1;
}

void ResetRecording()
{
    g_handlerRan = 0;
    g_handlerSawOnStack = 0;
    g_signalFrame = nullptr;
}

// Burn stack on the way down so the signal is taken from somewhere deep rather than from a fresh
// frame — the position that makes the difference between the two configurations matter.
//
__attribute__((noinline)) uint64_t DescendAndRaise(int depth)
{
    volatile uint64_t padding[128];
    padding[0] = static_cast<uint64_t>(depth);

    if (depth > 0)
    {
        padding[1] = DescendAndRaise(depth - 1);
    }
    else
    {
        raise(SIGUSR1);
        padding[1] = 0;
    }

    return padding[0] + padding[1];
}

bool WithinSegment(coop::Context* ctx, void const* addr)
{
    auto const* bottom = static_cast<uint8_t const*>(ctx->m_segment.Bottom());
    auto const* p = static_cast<uint8_t const*>(addr);
    return p >= bottom && p < bottom + ctx->m_segment.Size();
}

// Contexts here take a signal frame plus a deep recursion, so they get room. The point of the suite
// is that the frame does not land here, but the negative control deliberately puts it here once.
//
constexpr coop::SpawnConfiguration kRoomy = {.priority = 0, .stackSize = 512 * 1024};

struct Sigusr1Restore
{
    struct sigaction previous;

    Sigusr1Restore() { sigaction(SIGUSR1, nullptr, &previous); }
    ~Sigusr1Restore() { sigaction(SIGUSR1, &previous, nullptr); }
};

} // end anonymous namespace

// The size question, which is a portability trap before it is a sizing one: on glibc 2.34 and later
// SIGSTKSZ and MINSIGSTKSZ expand to sysconf calls, so code that used either as an array bound or a
// constexpr stopped compiling, and code that assumed 8192 stopped being right.
//
TEST(SignalStackTest, PreferredSizeIsResolvedAtRuntime)
{
    size_t const size = coop::SignalStack::PreferredSize();

    EXPECT_GE(size, static_cast<size_t>(MINSIGSTKSZ));
    EXPECT_GE(size, 128u * 1024u);
    EXPECT_EQ(size % static_cast<size_t>(sysconf(_SC_PAGESIZE)), 0u);
}

TEST(SignalStackTest, InstallsOnAnOrdinaryThread)
{
    bool activeInside = false;

    std::thread t([&]
    {
        coop::SignalStack signalStack;
        activeInside = coop::SignalStack::IsActive();

        stack_t current{};
        ASSERT_EQ(sigaltstack(nullptr, &current), 0);
        if (signalStack.IsInstalled())
        {
            // The public bounds describe the usable region, excluding the guard page.
            //
            EXPECT_EQ(signalStack.Bottom(), current.ss_sp);
            EXPECT_EQ(signalStack.Size(), current.ss_size);
        }
        else
        {
            EXPECT_EQ(signalStack.Bottom(), nullptr);
            EXPECT_EQ(signalStack.Size(), 0u);
        }
    });
    t.join();

    EXPECT_TRUE(activeInside);
}

// A cooperator's scheduler loop installs one for the life of the thread, so anything running in a
// context already has it.
//
TEST(SignalStackTest, InstalledForTheLifeOfACooperatorThread)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        EXPECT_TRUE(coop::SignalStack::IsActive());

        stack_t current{};
        ASSERT_EQ(sigaltstack(nullptr, &current), 0);
        EXPECT_EQ(current.ss_flags & SS_DISABLE, 0);
        EXPECT_GE(current.ss_size, coop::SignalStack::PreferredSize());

        // The alternate stack is somewhere else entirely, which is the whole claim.
        //
        EXPECT_FALSE(WithinSegment(ctx, current.ss_sp));
    });
}

// The direct proof: a signal taken at depth on a context stack, and a context that carries on
// afterwards.
//
TEST(SignalStackTest, HandlerTakenAtDepthRunsOffTheContextStack)
{
    Sigusr1Restore restore;
    ResetRecording();

    ASSERT_EQ(coop::RegisterOnAltStack(SIGUSR1, RecordingHandler), 0);

    test::RunInCooperator([](coop::Context* ctx)
    {
        ctx->GetCooperator()->Spawn(kRoomy, [](coop::Context* child)
        {
            uint64_t const before = 0xa5a5a5a5u;
            volatile uint64_t sentinel = before;

            uint64_t const sum = DescendAndRaise(24);
            EXPECT_GT(sum, 0u);

            ASSERT_EQ(g_handlerRan, 1);
            EXPECT_EQ(g_handlerSawOnStack, 1);

            // The kernel put its frame outside this context's segment.
            //
            EXPECT_FALSE(WithinSegment(child, g_signalFrame));

            // And the context is intact on the far side: its own stack was never written to, and it
            // can still yield and come back.
            //
            EXPECT_EQ(sentinel, before);
            child->Yield(true);
            EXPECT_EQ(sentinel, before);
        });
    });

    EXPECT_EQ(g_handlerRan, 1);
}

// The calibration case. Registered without SA_ONSTACK, the identical handler at the identical depth
// runs on the context's own stack — which is the failure this whole file exists to remove, and the
// evidence that the test above is measuring something rather than asserting a tautology.
//
// It is safe to do deliberately only because the context is given far more stack than the frame
// needs. In production, where 16KB is the default and a real workload has consumed most of it, this
// is the crash.
//
TEST(SignalStackTest, WithoutOnStackTheHandlerLandsOnTheContextStack)
{
    Sigusr1Restore restore;
    ResetRecording();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = RecordingHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;   // deliberately no SA_ONSTACK
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(sigaction(SIGUSR1, &sa, nullptr), 0);

    test::RunInCooperator([](coop::Context* ctx)
    {
        ctx->GetCooperator()->Spawn(kRoomy, [](coop::Context* child)
        {
            uint64_t const sum = DescendAndRaise(24);
            EXPECT_GT(sum, 0u);

            ASSERT_EQ(g_handlerRan, 1);
            EXPECT_EQ(g_handlerSawOnStack, 0);
            EXPECT_TRUE(WithinSegment(child, g_signalFrame));
        });
    });

    EXPECT_EQ(g_handlerRan, 1);
}
