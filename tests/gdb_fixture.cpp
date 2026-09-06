// A real runtime snapshot for GDB/core testing. --bare uses the same Context and assembly
// switch without a Cooperator, so saved-stack inspection can also run without io_uring.
#include <cstdlib>
#include <cstring>
#include <new>

#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/detail/asan_fiber.h"
#include "coop/detail/context_switch.h"
#include "coop/thread.h"

coop::Context* g_context = nullptr;
coop::Context* g_runningContext = nullptr;
coop::Context* g_badContext = nullptr;
void* g_hostSp = nullptr;
#if COOP_HAVE_ASAN
coop::detail::StackRegion g_hostStack;
#endif
bool g_bare = false;
coop::Coordinator* g_gate = nullptr;

extern "C" [[gnu::noinline]] void CoopGdbReady()
{
    asm volatile("" ::: "memory");
}

[[gnu::noinline]] void GdbParkLeaf(coop::Context* context)
{
    if (g_bare)
    {
        context->m_state = coop::SchedulerState::BLOCKED;
        COOP_ASAN_SWITCH_BEGIN(g_hostStack);
        ContextSwitch(&context->m_sp, g_hostSp, 0);
        COOP_ASAN_SWITCH_END();
        std::abort(); // The bare fixture intentionally never resumes the parked context.
    }
    else
    {
        g_gate->Acquire(context);
        g_gate->Release(context);
    }
}

[[gnu::noinline]] void GdbParkOuter(coop::Context* context)
{
    GdbParkLeaf(context);
    asm volatile("" ::: "memory");
}

struct BareContext : coop::Context
{
    BareContext() : Context(nullptr, coop::s_defaultConfiguration, nullptr, nullptr)
    {
        SetName("gdb-parked");
        m_cleanup = nullptr;
        m_heapTop = m_segment.Bottom();
        m_entry = GdbParkOuter;
    }
};

int main(int argc, char** argv)
{
    g_bare = argc > 1 && std::strcmp(argv[1], "--bare") == 0;
    if (g_bare)
    {
        // Intentionally leave parked contexts allocated until process exit: their normal
        // destruction requires a scheduler, which this bare switch fixture does not create.
        constexpr size_t bytes = sizeof(BareContext) + COOP_DEFAULT_STACK_SIZE;
        g_context = new (std::aligned_alloc(alignof(BareContext), bytes)) BareContext;
        g_badContext = new (std::aligned_alloc(alignof(BareContext), bytes)) BareContext;
        g_badContext->m_sp = reinterpret_cast<void*>(1);
        g_badContext->m_state = coop::SchedulerState::BLOCKED;
        g_context->m_sp = ContextInit(g_context->m_segment.Top(), g_context);
#if COOP_HAVE_ASAN
        g_hostStack = coop::detail::CurrentThreadStack();
#endif
        // The bare fixture bypasses the scheduler's normal sanitizer switch annotations.
        COOP_ASAN_SWITCH_BEGIN(coop::detail::StackOf(g_context));
        ContextSwitch(&g_hostSp, g_context->m_sp, 0);
        COOP_ASAN_SWITCH_END();
        CoopGdbReady();
        return 0;
    }

    coop::Cooperator cooperator;
    coop::Thread thread(&cooperator);
    cooperator.SubmitSync([&](coop::Context* context)
    {
        context->SetName("gdb-running");
        g_runningContext = context;
        coop::Coordinator gate;
        g_gate = &gate;
        gate.TryAcquire(context);
        cooperator.Spawn([](coop::Context* child)
        {
            child->SetName("gdb-parked");
            g_context = child;
            GdbParkOuter(child);
        });
        CoopGdbReady();
        gate.Release(context);
        cooperator.Shutdown();
    });
}
