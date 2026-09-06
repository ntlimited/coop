#include "introspect.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>

#include <spdlog/spdlog.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/detail/scheduler_state.h"
#include "coop/detail/stack_walk.h"

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

} // namespace

int CaptureStack(Context* ctx, uintptr_t* frames, int maxFrames)
{
    if (!ctx || !frames || maxFrames <= 0) return 0;

    detail::StackBounds bounds{
        reinterpret_cast<uintptr_t>(ctx->m_segment.Bottom()),
        reinterpret_cast<uintptr_t>(ctx->m_segment.Top())};

    Cooperator* co = ctx->GetCooperator();
    const bool running = co && co->Scheduled() == ctx;

    if (running)
    {
        // Our frame already contains the return address into the caller. Prepending
        // __builtin_return_address(0) would report that caller twice.
        //
        uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        return detail::WalkFrameChain(fp, bounds, frames, maxFrames);
    }

    uintptr_t pc, fp;
    uintptr_t savedSp = reinterpret_cast<uintptr_t>(ctx->m_sp);
    if (!detail::ReadSavedStack(savedSp, bounds, pc, fp)) return 0;

    frames[0] = pc;
    bounds.lo = savedSp;
    return 1 + detail::WalkFrameChain(fp, bounds, frames + 1, maxFrames - 1);
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
