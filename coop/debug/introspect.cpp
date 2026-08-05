#include "introspect.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>

#include <spdlog/spdlog.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/detail/scheduler_state.h"

namespace coop
{
namespace debug
{

namespace
{

const char* StateName(SchedulerState s)
{
    switch (s)
    {
        case SchedulerState::RUNNING:   return "running";
        case SchedulerState::YIELDED:   return "yielded";
        case SchedulerState::BLOCKED:   return "blocked";
        case SchedulerState::LAUNCHING: return "launching";
    }
    return "unknown";
}

// Recover (pc, fp) for the top frame of a *suspended* context from its saved stack pointer. The
// context switch (coop/detail/context_switch.S) pushes the callee-saved registers before storing
// rsp, so the frame pointer and return address sit at fixed offsets from m_sp.
//
bool TopFrameOf(void* savedSp, uintptr_t& pc, uintptr_t& fp)
{
    if (!savedSp) return false;
    auto* s = static_cast<uintptr_t*>(savedSp);
#if defined(__x86_64__)
    // pushed low->high: r15 r14 r13 r12 rbx rbp, then the call return address.
    fp = s[5];   // rbp
    pc = s[6];   // return address into the switch call site
#elif defined(__aarch64__)
    // stp x29,x30,[sp,#-96]! stores fp then lr at the lowest two slots.
    fp = s[0];   // x29
    pc = s[1];   // x30
#else
#error "Unsupported architecture for coop::debug::CaptureStack"
#endif
    return true;
}

} // namespace

int CaptureStack(Context* ctx, uintptr_t* frames, int maxFrames)
{
    if (!ctx || maxFrames <= 0) return 0;

    const uintptr_t lo = reinterpret_cast<uintptr_t>(ctx->m_segment.Bottom());
    const uintptr_t hi = reinterpret_cast<uintptr_t>(ctx->m_segment.Top());

    Cooperator* co = ctx->GetCooperator();
    const bool running = co && co->Scheduled() == ctx;

    uintptr_t pc, fp;
    if (running)
    {
        // The caller *is* this context — walk the live frame.
        //
        pc = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    }
    else if (!TopFrameOf(ctx->m_sp, pc, fp))
    {
        return 0;
    }

    int depth = 0;
    frames[depth++] = pc;

    // Frame-pointer walk, bounded to the context's own stack segment so a stale or bogus frame
    // pointer can never send us wandering through unrelated memory.
    //
    while (depth < maxFrames && fp >= lo && fp < hi && (fp & 0x7) == 0)
    {
        auto* frame = reinterpret_cast<uintptr_t*>(fp);
        uintptr_t ret = frame[1];
        if (ret == 0) break;
        frames[depth++] = ret;

        uintptr_t next = frame[0];
        if (next <= fp) break;   // frame pointers march toward the stack base
        fp = next;
    }

    return depth;
}

size_t Symbolize(uintptr_t pc, char* buf, size_t bufSize)
{
    if (bufSize == 0) return 0;

    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname)
    {
        int status = 0;
        char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        const char* name = (status == 0 && demangled) ? demangled : info.dli_sname;
        snprintf(buf, bufSize, "%s", name);
        free(demangled);
    }
    else if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname)
    {
        const char* lib = strrchr(info.dli_fname, '/');
        lib = lib ? lib + 1 : info.dli_fname;
        auto off = pc - reinterpret_cast<uintptr_t>(info.dli_fbase);
        snprintf(buf, bufSize, "%s+0x%lx", lib, static_cast<unsigned long>(off));
    }
    else
    {
        snprintf(buf, bufSize, "0x%lx", static_cast<unsigned long>(pc));
    }

    buf[bufSize - 1] = '\0';
    return strlen(buf);
}

void DumpContexts(Cooperator* co, FILE* out)
{
    if (!co) return;

    fprintf(out, "== coop context dump: cooperator '%s' ==\n", co->GetName());

    co->VisitContexts([&](Context* ctx) -> bool
    {
        uintptr_t frames[64];
        int depth = CaptureStack(ctx, frames, 64);

        const char* name = ctx->GetName();
        fprintf(out, "\ncontext '%s'  state=%s  frames=%d\n",
                name ? name : "(unnamed)", StateName(ctx->m_state), depth);

        char sym[256];
        for (int i = 0; i < depth; ++i)
        {
            Symbolize(frames[i], sym, sizeof(sym));
            fprintf(out, "  #%-2d 0x%016lx  %s\n", i,
                    static_cast<unsigned long>(frames[i]), sym);
            if (strstr(sym, "CoopContextEntry")) break;   // stack base; drop the sentinel above it
        }
        return true;
    });

    fflush(out);
}

void DumpContexts(Cooperator* co)
{
    if (!co) return;

    co->VisitContexts([&](Context* ctx) -> bool
    {
        uintptr_t frames[64];
        int depth = CaptureStack(ctx, frames, 64);

        std::string line;
        char sym[256];
        for (int i = 0; i < depth; ++i)
        {
            Symbolize(frames[i], sym, sizeof(sym));
            if (i) line += " <- ";
            line += sym;
            if (strstr(sym, "CoopContextEntry")) break;   // stack base; drop the sentinel above it
        }

        spdlog::warn("coop context '{}' state={} frames={}: {}",
                     ctx->GetName() ? ctx->GetName() : "(unnamed)",
                     StateName(ctx->m_state), depth, line);
        return true;
    });
}

} // end namespace coop::debug
} // end namespace coop
