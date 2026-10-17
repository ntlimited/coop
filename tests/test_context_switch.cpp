#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include <gtest/gtest.h>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#include "coop/detail/asan_fiber.h"
#include "coop/detail/context_switch.h"

namespace
{

// Exercise the production switch without constructing a scheduler or requiring io_uring.
// Observations are recorded on the fiber and asserted after returning to the native stack.
//
struct BareContext
{
    static constexpr size_t kStackSize = 64 * 1024;
    std::unique_ptr<uint8_t[]> stack{new uint8_t[kStackSize]};
    void* sp = nullptr;
    void* nativeSp = nullptr;
    coop::detail::StackRegion nativeRegion;
    void (*entry)(BareContext*) = nullptr;
    void* data = nullptr;
    bool completed = false;

    explicit BareContext(void (*fn)(BareContext*), void* arg) : entry(fn), data(arg)
    {
#if COOP_HAVE_ASAN
        nativeRegion = coop::detail::CurrentThreadStack();
#endif
        auto top = reinterpret_cast<uintptr_t>(stack.get() + kStackSize) & ~uintptr_t{15};
        sp = ContextInit(reinterpret_cast<void*>(top), reinterpret_cast<coop::Context*>(this));
    }

    int Resume(int result)
    {
        COOP_ASAN_SWITCH_BEGIN((coop::detail::StackRegion{stack.get(), kStackSize}));
        int value = ContextSwitch(&nativeSp, sp, result);
        COOP_ASAN_SWITCH_END();
        return value;
    }

    int Yield(int result)
    {
        COOP_ASAN_SWITCH_BEGIN(nativeRegion);
        int value = ContextSwitch(&sp, nativeSp, result);
        COOP_ASAN_SWITCH_END();
        return value;
    }
};

constexpr int kRounds = 32;

} // end anonymous namespace

// The assembly trampoline expects this symbol. This standalone executable supplies a minimal
// entry point instead of linking the runtime's Context lifecycle and its scheduler dependencies.
//
extern "C" void CoopContextEntry(coop::Context* opaque)
{
    COOP_ASAN_SWITCH_FIRST_ENTRY();
    auto* context = reinterpret_cast<BareContext*>(opaque);
    context->entry(context);
    context->completed = true;
    COOP_ASAN_SWITCH_BEGIN_FINAL(context->nativeRegion);
    ContextSwitch(&context->sp, context->nativeSp, -1);
    std::abort();
}

#if defined(__x86_64__)

namespace
{

struct VectorProbe
{
    std::array<uint64_t, 8> wanted;
    std::array<std::array<uint64_t, 8>, kRounds> observed{};
};

// Opaque register operands keep the vector genuinely live across the ordinary function call.
// The compiler must spill/reload it according to SysV; coop need not save caller-saved vectors.
//
__attribute__((target("avx"))) void ProbeAvx(BareContext* context)
{
    auto& probe = *static_cast<VectorProbe*>(context->data);
    for (int i = 0; i < kRounds; i++)
    {
        __m256i value = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(probe.wanted.data()));
        asm volatile("" : "+x"(value));
        context->Yield(42);
        asm volatile("" : "+x"(value));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(probe.observed[i].data()), value);
    }
}

__attribute__((target("avx512f"))) void ProbeAvx512(BareContext* context)
{
    auto& probe = *static_cast<VectorProbe*>(context->data);
    for (int i = 0; i < kRounds; i++)
    {
        __m512i value = _mm512_loadu_si512(probe.wanted.data());
        asm volatile("" : "+v"(value));
        context->Yield(42);
        asm volatile("" : "+v"(value));
        _mm512_storeu_si512(probe.observed[i].data(), value);
    }
}

void CheckLiveVectors(void (*entry)(BareContext*), size_t lanes)
{
    VectorProbe a{{11, 22, 33, 44, 55, 66, 77, 88}};
    VectorProbe b{{99, 111, 222, 333, 444, 555, 666, 777}};
    BareContext first(entry, &a), second(entry, &b);
    for (int i = 0; i <= kRounds; i++)
    {
        EXPECT_EQ(first.Resume(0), i < kRounds ? 42 : -1);
        EXPECT_EQ(second.Resume(0), i < kRounds ? 42 : -1);
    }
    EXPECT_TRUE(first.completed);
    EXPECT_TRUE(second.completed);
    for (int i = 0; i < kRounds; i++)
    {
        for (size_t lane = 0; lane < lanes; lane++)
        {
            EXPECT_EQ(a.observed[i][lane], a.wanted[lane]);
            EXPECT_EQ(b.observed[i][lane], b.wanted[lane]);
        }
    }
}

} // end anonymous namespace

TEST(ContextSwitchTest, CompilerPreservesLiveAvxValues)
{
    if (!__builtin_cpu_supports("avx"))
    {
        GTEST_SKIP() << "AVX is unavailable";
    }
    CheckLiveVectors(ProbeAvx, 4);
}

TEST(ContextSwitchTest, CompilerPreservesLiveAvx512Values)
{
    if (!__builtin_cpu_supports("avx512f"))
    {
        GTEST_SKIP() << "AVX-512 is unavailable";
    }
    CheckLiveVectors(ProbeAvx512, 8);
}

#endif

#if defined(__aarch64__)

extern "C" int CoopTestWithDRegisters(
    uint64_t const* expected, uint64_t* observed, int (*callback)(void*), void* arg);

namespace
{

struct RegisterProbe
{
    std::array<uint64_t, 8> wanted;
    std::array<std::array<uint64_t, 8>, kRounds> observed{};
    std::array<int, kRounds> results{};
};

int YieldWithDRegisters(void* opaque)
{
    return static_cast<BareContext*>(opaque)->Yield(42);
}

void ProbeDRegisters(BareContext* context)
{
    auto& probe = *static_cast<RegisterProbe*>(context->data);
    for (int i = 0; i < kRounds; i++)
    {
        probe.results[i] = CoopTestWithDRegisters(
            probe.wanted.data(), probe.observed[i].data(), YieldWithDRegisters, context);
    }
}

} // end anonymous namespace

TEST(ContextSwitchTest, PreservesAllArmCalleeSavedSimdRegisters)
{
    RegisterProbe a{{0x100000001, 0x200000002, 0x300000003, 0x400000004,
                     0x500000005, 0x600000006, 0x700000007, 0x800000008}};
    RegisterProbe b{{0xfedcba9876543210, 0xedcba98765432101, 0xdcba987654321012,
                     0xcba9876543210123, 0xba98765432101234, 0xa987654321012345,
                     0x9876543210123456, 0x8765432101234567}};
    BareContext first(ProbeDRegisters, &a), second(ProbeDRegisters, &b);
    for (int i = 0; i <= kRounds; i++)
    {
        EXPECT_EQ(first.Resume(71), i < kRounds ? 42 : -1);
        EXPECT_EQ(second.Resume(72), i < kRounds ? 42 : -1);
    }
    EXPECT_TRUE(first.completed);
    EXPECT_TRUE(second.completed);
    for (int i = 0; i < kRounds; i++)
    {
        EXPECT_EQ(a.observed[i], a.wanted) << "first context, round " << i;
        EXPECT_EQ(b.observed[i], b.wanted) << "second context, round " << i;
        EXPECT_EQ(a.results[i], 71);
        EXPECT_EQ(b.results[i], 72);
    }
}

#endif
