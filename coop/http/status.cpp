#include "status.h"
#include "server.h"
#include "connection.h"

#include <cstdint>
#include <cxxabi.h>
#include <dlfcn.h>
#include <string>

#include <cstdio>
#include <cstring>
#include <map>

#include "coop/cooperator.h"
#include "coop/context.h"
#include "coop/debug/introspect.h"
#include "coop/detail/scheduler_state.h"
#include "coop/epoch/epoch.h"
#include "coop/perf/counters.h"
#include "coop/perf/patch.h"
#include "coop/perf/sampler.h"

namespace coop
{
namespace http
{

namespace
{

// Minimal JSON writer that appends to a string reference. Handles comma separation via
// depth-tracked first-element flags.
//
struct JsonWriter
{
    static constexpr int MAX_DEPTH = 32;

    std::string& m_out;
    bool m_first[MAX_DEPTH];
    int m_depth;

    JsonWriter(std::string& out)
    : m_out(out)
    , m_depth(0)
    {
        m_first[0] = true;
    }

    void Comma()
    {
        if (!m_first[m_depth])
        {
            m_out += ',';
        }
        m_first[m_depth] = false;
    }

    void BeginObject()
    {
        Comma();
        m_out += '{';
        m_depth++;
        m_first[m_depth] = true;
    }

    void EndObject()
    {
        m_depth--;
        m_out += '}';
    }

    void BeginArray()
    {
        Comma();
        m_out += '[';
        m_depth++;
        m_first[m_depth] = true;
    }

    void EndArray()
    {
        m_depth--;
        m_out += ']';
    }

    void Key(const char* key)
    {
        Comma();
        m_out += '"';
        m_out += key;
        m_out += "\":";
        // Mark as first so the value itself doesn't emit a comma
        //
        m_first[m_depth] = true;
    }

    void String(const char* val)
    {
        Comma();
        m_out += '"';
        // Escape special characters
        //
        for (const char* p = val; *p; p++)
        {
            switch (*p)
            {
                case '"':  m_out += "\\\""; break;
                case '\\': m_out += "\\\\"; break;
                case '\n': m_out += "\\n";  break;
                case '\r': m_out += "\\r";  break;
                case '\t': m_out += "\\t";  break;
                default:   m_out += *p;     break;
            }
        }
        m_out += '"';
    }

    void Int(int64_t val)
    {
        Comma();
        m_out += std::to_string(val);
    }

    void UInt(uint64_t val)
    {
        Comma();
        m_out += std::to_string(val);
    }

    void Bool(bool val)
    {
        Comma();
        m_out += val ? "true" : "false";
    }

    void Null()
    {
        Comma();
        m_out += "null";
    }
};

const char* StateString(SchedulerState state)
{
    switch (state)
    {
        case SchedulerState::RUNNING: return "running";
        case SchedulerState::YIELDED: return "yielded";
        case SchedulerState::BLOCKED: return "blocked";
        case SchedulerState::LAUNCHING: return "launching";
    }
    return "unknown";
}

void SerializeContext(JsonWriter& w, Context* ctx)
{
    w.BeginObject();

    w.Key("name");
    w.String(ctx->GetName() ? ctx->GetName() : "(unnamed)");

    w.Key("state");
    w.String(StateString(ctx->m_state));

    w.Key("killed");
    w.Bool(ctx->IsKilled());

    w.Key("priority");
    w.Int(ctx->m_priority);

    w.Key("statistics");
    w.BeginObject();
    w.Key("ticks");
    w.UInt(ctx->m_statistics.ticks);
    w.Key("yields");
    w.UInt(ctx->m_statistics.yields);
    w.Key("blocks");
    w.UInt(ctx->m_statistics.blocks);
    w.Key("ioSubmits");
    w.UInt(ctx->m_statistics.ioSubmits);
    w.Key("ioCompletes");
    w.UInt(ctx->m_statistics.ioCompletes);
    w.Key("samples");
    w.UInt(ctx->m_statistics.samples);
    w.EndObject();

    w.Key("children");
    w.BeginArray();
    ctx->m_children.Visit([&](Context* child) -> bool
    {
        SerializeContext(w, child);
        return true;
    });
    w.EndArray();

    w.EndObject();
}

void SerializeCooperatorStatus(JsonWriter& w, Cooperator* co)
{
    w.BeginObject();

    w.Key("name");
    w.String(co->GetName());

    w.Key("contextsCount");
    w.UInt(co->ContextsCount());

    w.Key("yieldedCount");
    w.UInt(co->YieldedCount());

    w.Key("blockedCount");
    w.UInt(co->BlockedCount());

    w.Key("ticks");
    w.Int(co->GetTicks());

    w.Key("contexts");
    w.BeginArray();
    co->VisitContexts([&](Context* ctx) -> bool
    {
        if (!ctx->Parent())
        {
            SerializeContext(w, ctx);
        }
        return true;
    });
    w.EndArray();

    w.EndObject();
}

std::string GenerateStatusJson(Cooperator* co)
{
    std::string out;
    out.reserve(4096);
    JsonWriter w(out);
    SerializeCooperatorStatus(w, co);
    return out;
}

void HandleStatus(ConnectionBase& conn)
{
    std::string body = GenerateStatusJson(conn.GetCooperator());
    conn.Send(200, "application/json", body);
}

void SerializePerfCounters(JsonWriter& w, Cooperator* co)
{
    w.BeginObject();

    w.Key("name");
    w.String(co->GetName());

    w.Key("counters");
    w.BeginObject();

#if COOP_PERF_MODE > 0
    auto& counters = co->GetPerfCounters();
    for (size_t i = 0; i < static_cast<size_t>(perf::Counter::COUNT); i++)
    {
        auto c = static_cast<perf::Counter>(i);
        w.Key(perf::CounterName(c));
        w.UInt(counters.Get(c));
    }
#else
    (void)co;
#endif

    w.EndObject();
    w.EndObject();
}

// Parse a comma-separated family string (e.g. "scheduler,scan,txn") into a Family bitmask.
//
perf::Family ParseFamilies(const char* data, size_t len)
{
    perf::Family result{};
    const char* p = data;
    const char* end = data + len;
    while (p < end)
    {
        const char* comma = static_cast<const char*>(memchr(p, ',', end - p));
        size_t tok = (comma ? comma : end) - p;
        std::string_view name(p, tok);
        for (size_t i = 0; i < perf::kFamilyCount; i++)
        {
            if (name == perf::FamilyName(perf::s_allFamilies[i]))
            {
                result = result | perf::s_allFamilies[i];
                break;
            }
        }
        p += tok + 1;
    }
    return result;
}

std::string GeneratePerfJson(Cooperator* co)
{
    std::string out;
    out.reserve(2048);
    JsonWriter w(out);

    w.BeginObject();

    w.Key("mode");
    w.Int(COOP_PERF_MODE);

    w.Key("enabled");
    w.Bool(perf::IsEnabled());

    w.Key("probeCount");
    w.UInt(perf::ProbeCount());

    perf::Family enabled = perf::EnabledFamilies();

    w.Key("enabledFamilies");
    w.UInt(static_cast<uint64_t>(enabled));

    w.Key("families");
    w.BeginObject();
    for (size_t i = 0; i < perf::kFamilyCount; i++)
    {
        perf::Family f = perf::s_allFamilies[i];
        w.Key(perf::FamilyName(f));
        w.BeginObject();
        w.Key("bit");
        w.UInt(static_cast<uint64_t>(f));
        w.Key("enabled");
        w.Bool(perf::HasFamily(enabled, f));
        w.EndObject();
    }
    w.EndObject();

    w.Key("counters");
    w.BeginObject();

#if COOP_PERF_MODE > 0
    auto& counters = co->GetPerfCounters();
    for (size_t i = 0; i < static_cast<size_t>(perf::Counter::COUNT); i++)
    {
        auto c = static_cast<perf::Counter>(i);
        w.Key(perf::CounterName(c));
        w.UInt(counters.Get(c));
    }
#else
    (void)co;
#endif

    w.EndObject();
    w.EndObject();
    return out;
}

void HandlePerf(ConnectionBase& conn)
{
    std::string body = GeneratePerfJson(conn.GetCooperator());
    conn.Send(200, "application/json", body);
}

void HandlePerfEnable(ConnectionBase& conn)
{
    perf::Family families = perf::Family::All;
    while (auto* name = conn.NextArgName())
    {
        if (name == std::string_view("families"))
        {
            if (auto* chunk = conn.ReadArgValue())
                families = ParseFamilies(
                    static_cast<const char*>(chunk->data), chunk->size);
        }
    }
    perf::Enable(families);
    conn.Send(200, "application/json", "{\"ok\":true}");
}

void HandlePerfDisable(ConnectionBase& conn)
{
    perf::Family families = perf::Family::All;
    while (auto* name = conn.NextArgName())
    {
        if (name == std::string_view("families"))
        {
            if (auto* chunk = conn.ReadArgValue())
                families = ParseFamilies(
                    static_cast<const char*>(chunk->data), chunk->size);
        }
    }
    perf::Disable(families);
    conn.Send(200, "application/json", "{\"ok\":true}");
}

// ---- Sampler API ----

void HandleSampler(ConnectionBase& conn)
{
    std::string out;
    out.reserve(256);
    JsonWriter w(out);
    w.BeginObject();
    w.Key("sampling");
    w.Bool(perf::IsSampling());
    w.Key("stacks");
    w.Bool(perf::IsStackMode());
    w.Key("hz");
    w.Int(perf::SamplingHz());
    w.Key("totalSamples");
    w.UInt(perf::TotalSamples());
    w.Key("capacity");
    w.UInt(perf::SampleCapacity());
    w.Key("stackSubsample");
    w.Int(perf::StackSubsample());
    w.EndObject();
    conn.Send(200, "application/json", out.data(), out.size());
}

void HandleSamplerStart(ConnectionBase& conn)
{
    // Read hz and stacks from query string
    //
    int hz = 99;
    bool stacks = false;
    while (auto* name = conn.NextArgName())
    {
        if (name == std::string_view("hz"))
        {
            if (auto* chunk = conn.ReadArgValue())
            {
                char buf[16] = {};
                size_t len = chunk->size < sizeof(buf) - 1 ? chunk->size : sizeof(buf) - 1;
                memcpy(buf, chunk->data, len);
                int val = atoi(buf);
                if (val > 0) hz = val;
            }
        }
        else if (name == std::string_view("stacks"))
        {
            if (auto* chunk = conn.ReadArgValue())
            {
                stacks = chunk->size > 0 &&
                    static_cast<const char*>(chunk->data)[0] == '1';
            }
        }
    }
    perf::ResetSamples();
    bool ok = perf::StartSampling(hz, stacks);
    conn.Send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void HandleSamplerStop(ConnectionBase& conn)
{
    perf::StopSampling();
    conn.Send(200, "application/json", "{\"ok\":true}");
}

void SerializeSampleContext(JsonWriter& w, const char* ctxName, const char* coopName)
{
    w.Key("context");
    if (ctxName) w.String(ctxName);
    else w.Null();
    w.Key("cooperator");
    if (coopName) w.String(coopName);
    else w.Null();
}

static void SerializePcSamples(JsonWriter& w, std::string& out)
{
    static constexpr size_t MAX_READ = 8192;
    auto* samples = new perf::Sample[MAX_READ];
    size_t count = perf::ReadSamples(samples, MAX_READ);

    out.reserve(out.size() + count * 48 + 64);
    w.Key("count");
    w.UInt(count);
    w.Key("samples");
    w.BeginArray();
    for (size_t i = 0; i < count; i++)
    {
        w.BeginObject();
        w.Key("pc");
        char pcBuf[20];
        snprintf(pcBuf, sizeof(pcBuf), "0x%lx", samples[i].pc);
        w.String(pcBuf);
        SerializeSampleContext(w,
            samples[i].context ? samples[i].context->GetName() : nullptr,
            samples[i].cooperator ? samples[i].cooperator->GetName() : nullptr);
        w.Key("ts");
        w.UInt(samples[i].timestamp);
        w.EndObject();
    }
    w.EndArray();
    delete[] samples;
}

static void SerializeStackSamples(JsonWriter& w, std::string& out)
{
    static constexpr size_t MAX_READ = 2048;
    auto* samples = new perf::StackSample[MAX_READ];
    size_t count = perf::ReadStackSamples(samples, MAX_READ);

    out.reserve(out.size() + count * 200 + 64);
    w.Key("stackCount");
    w.UInt(count);
    w.Key("stackSamples");
    w.BeginArray();
    for (size_t i = 0; i < count; i++)
    {
        w.BeginObject();
        w.Key("frames");
        w.BeginArray();
        for (int f = 0; f < samples[i].depth; f++)
        {
            char pcBuf[20];
            snprintf(pcBuf, sizeof(pcBuf), "0x%lx", samples[i].frames[f]);
            w.String(pcBuf);
        }
        w.EndArray();
        SerializeSampleContext(w,
            samples[i].context ? samples[i].context->GetName() : nullptr,
            samples[i].cooperator ? samples[i].cooperator->GetName() : nullptr);
        w.Key("ts");
        w.UInt(samples[i].timestamp);
        w.EndObject();
    }
    w.EndArray();
    delete[] samples;
}

void HandleSamplerSamples(ConnectionBase& conn)
{
    bool stackMode = perf::IsStackMode();

    std::string out;
    JsonWriter w(out);
    w.BeginObject();
    w.Key("stacks");
    w.Bool(stackMode);

    // PC ring is always populated (full-rate RIP capture).
    //
    SerializePcSamples(w, out);

    // In stack mode, also include the subsampled stack traces.
    //
    if (stackMode)
    {
        SerializeStackSamples(w, out);
    }

    w.EndObject();
    conn.Send(200, "application/json", out.data(), out.size());
}

void HandleSymbolize(ConnectionBase& conn)
{
    // Read POST body: comma-separated hex PCs like "0x1234,0x5678,..."
    //
    std::string body;
    body.reserve(4096);
    while (auto* chunk = conn.ReadBody())
    {
        body.append(static_cast<const char*>(chunk->data), chunk->size);
        if (chunk->complete) break;
        if (body.size() > 65536) break; // safety limit
    }

    std::string out;
    out.reserve(body.size() * 4);
    JsonWriter w(out);
    w.BeginObject();
    w.Key("symbols");
    w.BeginObject();

    // Parse comma-separated hex addresses
    //
    const char* p = body.c_str();
    while (*p)
    {
        // Skip whitespace and commas
        //
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;

        // Parse hex address
        //
        char* end;
        uintptr_t pc = strtoull(p, &end, 16);
        if (end == p) break;
        p = end;

        // Look up symbol via dladdr
        //
        char pcKey[20];
        snprintf(pcKey, sizeof(pcKey), "0x%lx", pc);

        Dl_info info;
        w.Key(pcKey);
        if (dladdr(reinterpret_cast<void*>(pc), &info))
        {
            if (info.dli_sname)
            {
                int status;
                char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                w.String(status == 0 ? demangled : info.dli_sname);
                free(demangled);
            }
            else if (info.dli_fname)
            {
                // No symbol name but we know the shared object — report lib+offset
                // rounded to 64-byte granularity to merge nearby instructions
                //
                const char* libname = info.dli_fname;
                const char* slash = strrchr(libname, '/');
                if (slash) libname = slash + 1;
                auto offset = (pc - reinterpret_cast<uintptr_t>(info.dli_fbase)) & ~0x3FUL;
                char buf[256];
                snprintf(buf, sizeof(buf), "%s+0x%lx", libname, offset);
                w.String(buf);
            }
            else
            {
                w.String("???");
            }
        }
        else
        {
            w.String("???");
        }
    }

    w.EndObject();
    w.EndObject();
    conn.Send(200, "application/json", out.data(), out.size());
}

// ---- Multi-cooperator API ----
//
// Provides a unified view of all cooperators in the process. For the local cooperator (the one
// running the status server), data is read directly. For remote cooperators, counter values are
// read cross-thread — uint64_t reads are tear-free on x86-64, acceptable for observability.
// Context trees are gathered via SubmitSync to serialize access on the owning thread.
//

void HandleCooperators(ConnectionBase& conn)
{
    Cooperator* local = conn.GetCooperator();
    std::string out;
    out.reserve(8192);
    JsonWriter w(out);

    w.BeginObject();
    w.Key("cooperators");
    w.BeginArray();

    Cooperator::VisitRegistry([&](Cooperator* co) -> bool
    {
        if (co == local)
        {
            // Local cooperator — safe to read directly
            //
            SerializeCooperatorStatus(w, co);
        }
        else
        {
            // Remote cooperator — gather context tree via SubmitSync for thread safety.
            // SubmitSync blocks the current context (not the thread) until the remote
            // cooperator executes our lambda.
            //
            // Note: we must drop the registry lock before calling SubmitSync, because the
            // remote cooperator's DrainSubmissions runs under its own thread and doesn't
            // touch the registry lock. However, VisitRegistry holds the lock for the
            // duration of the visit. To avoid deadlock concerns, we collect remote
            // cooperator pointers first, then query them outside the lock.
            //
            // For now, read counters and summary stats cross-thread (tear-free uint64_t on
            // x86-64). Context tree is omitted for remote cooperators in the registry walk
            // to avoid the lock ordering issue. The /api/cooperators/<name>/status endpoint
            // uses SubmitSync for full context tree access.
            //
            w.BeginObject();
            w.Key("name");
            w.String(co->GetName());
            w.Key("contextsCount");
            w.UInt(co->ContextsCount());
            w.Key("yieldedCount");
            w.UInt(co->YieldedCount());
            w.Key("blockedCount");
            w.UInt(co->BlockedCount());
            w.Key("ticks");
            w.Int(co->GetTicks());
            w.Key("contexts");
            w.BeginArray();
            w.EndArray();
            w.EndObject();
        }
        return true;
    });

    w.EndArray();
    w.EndObject();
    conn.Send(200, "application/json", out.data(), out.size());
}

void HandleCooperatorsPerf(ConnectionBase& conn)
{
    std::string out;
    out.reserve(4096);
    JsonWriter w(out);

    w.BeginObject();

    w.Key("mode");
    w.Int(COOP_PERF_MODE);

    w.Key("enabled");
    w.Bool(perf::IsEnabled());

    w.Key("probeCount");
    w.UInt(perf::ProbeCount());

    w.Key("cooperators");
    w.BeginArray();

    // Counter reads are tear-free on x86-64 — safe to read cross-thread for observability.
    //
    Cooperator::VisitRegistry([&](Cooperator* co) -> bool
    {
        SerializePerfCounters(w, co);
        return true;
    });

    w.EndArray();
    w.EndObject();
    conn.Send(200, "application/json", out.data(), out.size());
}

// ---- Epoch API ----

void HandleEpoch(ConnectionBase& conn)
{
    auto* mgr = epoch::GetManager();
    if (!mgr)
    {
        conn.Send(200, "application/json", "{\"error\":\"no epoch manager\"}");
        return;
    }

    std::string out;
    out.reserve(512);
    JsonWriter w(out);

    w.BeginObject();

    uint64_t current = mgr->Current().Value();
    epoch::Epoch safe = mgr->SafeEpoch();
    uint64_t safeVal = safe.Value();

    w.Key("current");
    w.UInt(current);
    w.Key("safe");
    w.UInt(safeVal);
    w.Key("lag");
    w.UInt(safe.IsAlive() ? 0 : current - safeVal);
    w.Key("pendingRetire");
    w.UInt(mgr->PendingCount());

    auto snap = mgr->SnapshotPins();
    w.Key("pins");
    w.BeginObject();
    w.Key("traversal");
    w.UInt(snap.traversalPins);
    w.Key("application");
    w.UInt(snap.applicationPins);
    w.EndObject();

    w.EndObject();
    conn.Send(200, "application/json", out.data(), out.size());
}

void HandleEpochAll(ConnectionBase& conn)
{
    auto* localMgr = epoch::GetManager();

    std::string out;
    out.reserve(2048);
    JsonWriter w(out);

    w.BeginObject();

    // Global safe epoch is the minimum across all cooperator watermarks.
    //
    w.Key("globalSafe");
    if (localMgr)
    {
        epoch::Epoch safe = localMgr->SafeEpoch();
        w.UInt(safe.Value());
    }
    else
    {
        w.UInt(0);
    }

    w.Key("cooperators");
    w.BeginArray();

    Cooperator::VisitRegistry([&](Cooperator* co) -> bool
    {
        w.BeginObject();
        w.Key("name");
        w.String(co->GetName());
        w.Key("watermark");
        w.UInt(co->GetEpochWatermark().Value());
        w.EndObject();
        return true;
    });

    w.EndArray();
    w.EndObject();
    conn.Send(200, "application/json", out.data(), out.size());
}

// ---- Context stacks ----
//
// The stackful payoff over HTTP: every context on this cooperator, with the call stack it is
// currently parked in. For a blocked context that stack names the exact wait it is stuck on — curl
// this on a wedged server and it explains itself. Two shapes: JSON for tooling, plaintext for eyes.

void HandleContextStacks(ConnectionBase& conn)
{
    Cooperator* co = conn.GetCooperator();

    std::string out;
    out.reserve(16384);
    JsonWriter w(out);

    w.BeginObject();
    w.Key("cooperator");
    w.String(co->GetName());
    w.Key("contexts");
    w.BeginArray();

    co->VisitContexts([&](Context* ctx) -> bool
    {
        w.BeginObject();
        w.Key("name");
        w.String(ctx->GetName() ? ctx->GetName() : "(unnamed)");
        w.Key("state");
        w.String(StateString(ctx->m_state));

        w.Key("frames");
        w.BeginArray();

        uintptr_t frames[64];
        int depth = debug::CaptureStack(ctx, frames, 64);
        char sym[256];
        for (int i = 0; i < depth; i++)
        {
            debug::Symbolize(frames[i], sym, sizeof(sym));
            w.BeginObject();
            char pc[20];
            snprintf(pc, sizeof(pc), "0x%lx", static_cast<unsigned long>(frames[i]));
            w.Key("pc");
            w.String(pc);
            w.Key("fn");
            w.String(sym);
            w.EndObject();
            if (strstr(sym, "CoopContextEntry")) break;   // stack base; drop the sentinel above it
        }

        w.EndArray();
        w.EndObject();
        return true;
    });

    w.EndArray();
    w.EndObject();
    conn.Send(200, "application/json", out.data(), out.size());
}

void HandleContextDump(ConnectionBase& conn)
{
    char* buf = nullptr;
    size_t sz = 0;
    FILE* f = open_memstream(&buf, &sz);
    if (!f)
    {
        conn.Send(500, "text/plain", "open_memstream failed\n");
        return;
    }

    debug::DumpContexts(conn.GetCooperator(), f);
    fclose(f);

    conn.Send(200, "text/plain", buf, sz);
    free(buf);
}

// ---- Health ----
//
// The one-glance endpoint: is this cooperator alive and what is it doing. `loops` is the scheduler
// heartbeat — poll twice and if it moved, the loop is progressing; if it is frozen while `blocked`
// is high, something is wedging the thread (pair with /api/contexts/dump to see what).
//
void HandleHealth(ConnectionBase& conn)
{
    Cooperator* co = conn.GetCooperator();
    Context* sched = co->Scheduled();

    std::string out;
    out.reserve(512);
    JsonWriter w(out);

    w.BeginObject();
    w.Key("cooperator"); w.String(co->GetName());
    w.Key("cpu");        w.Int(co->CpuId());
    w.Key("numa");       w.Int(co->NumaNode());
    w.Key("loops");      w.UInt(co->Loops());
    w.Key("scheduled");
    if (sched && sched->GetName()) w.String(sched->GetName()); else w.Null();

    w.Key("contexts");
    w.BeginObject();
    w.Key("total");     w.UInt(co->ContextsCount());
    w.Key("yielded");   w.UInt(co->YieldedCount());
    w.Key("blocked");   w.UInt(co->BlockedCount());
    w.Key("running");   w.UInt(sched ? 1 : 0);
    w.Key("nonDaemon"); w.UInt(co->NonDaemonContexts());
    w.EndObject();

    w.EndObject();
    conn.Send(200, "application/json", out.data(), out.size());
}

// ---- Sampler: folded-stack export ----
//
// Emit the sampler's stack ring as Brendan-Gregg collapsed/folded stacks (root;...;leaf<space>count),
// the input format for flamegraph.pl and speedscope. Turns the built-in profiler into standard
// tooling: /api/sampler/start?stacks=1, apply load, then curl /api/sampler/folded > out.folded.
// Symbolizing raw PCs is safe even for since-exited contexts — code addresses do not move.
//
void HandleSamplerFolded(ConnectionBase& conn)
{
    static constexpr size_t MAX_READ = 2048;
    auto* samples = new perf::StackSample[MAX_READ];
    size_t count = perf::ReadStackSamples(samples, MAX_READ);

    std::map<std::string, uint64_t> folded;
    char sym[256];
    for (size_t i = 0; i < count; i++)
    {
        std::string line;
        for (int f = samples[i].depth - 1; f >= 0; f--)   // root (outermost) to leaf
        {
            debug::Symbolize(samples[i].frames[f], sym, sizeof(sym));
            if (!line.empty()) line += ';';
            line += sym;
        }
        if (!line.empty()) folded[line]++;
    }
    delete[] samples;

    std::string out;
    out.reserve(folded.size() * 64);
    for (auto& kv : folded)
    {
        out += kv.first;
        out += ' ';
        out += std::to_string(kv.second);
        out += '\n';
    }
    conn.Send(200, "text/plain", out.data(), out.size());
}

// The status app's own private route table -- exact path to handler. Not exposed; StatusDispatch
// walks it. Keeping it here (rather than in coop's server) is the point of the strip: routing is the
// application's, and the dashboard is just another application.
//
struct StatusRoute { const char* path; void (*handler)(ConnectionBase&); };

StatusRoute s_statusRoutes[] = {
    {"/api/status",         HandleStatus},
    {"/api/perf",           HandlePerf},
    {"/api/perf/enable",    HandlePerfEnable},
    {"/api/perf/disable",   HandlePerfDisable},
    {"/api/sampler",        HandleSampler},
    {"/api/sampler/start",  HandleSamplerStart},
    {"/api/sampler/stop",   HandleSamplerStop},
    {"/api/sampler/samples", HandleSamplerSamples},
    {"/api/sampler/folded",  HandleSamplerFolded},
    {"/api/sampler/symbolize", HandleSymbolize},
    {"/api/health",          HandleHealth},
    {"/api/cooperators",       HandleCooperators},
    {"/api/cooperators/perf",  HandleCooperatorsPerf},
    {"/api/epoch",             HandleEpoch},
    {"/api/epoch/all",         HandleEpochAll},
    {"/api/contexts/stacks",   HandleContextStacks},
    {"/api/contexts/dump",     HandleContextDump},
};

// Top-level handler for the standalone status server: the dashboard API, then static files, then
// 404. searchPaths (a null-terminated const char* array) rides through userData.
//
void StatusServerHandler(ConnectionBase& conn, void* userData)
{
    auto* req = conn.GetRequestLine();
    std::string_view path = req->path;

    if (StatusDispatch(conn, path)) return;

    auto* searchPaths = static_cast<const char* const*>(userData);
    if (searchPaths && ServeFile(conn, path, searchPaths)) return;

    conn.Send(404, "text/plain", "Not Found\n");
}

} // end anonymous namespace

bool StatusDispatch(ConnectionBase& conn, std::string_view path)
{
    for (auto& route : s_statusRoutes)
    {
        if (path == route.path)
        {
            route.handler(conn);
            return true;
        }
    }
    return false;
}

void SpawnStatusServer(Cooperator* co, int port,
                       const char* const* searchPaths /* = nullptr */)
{
    co->Spawn([=](Context* ctx)
    {
        ServerConfiguration config;
        config.port = port;
        config.name = "HttpStatusServer";
        config.handler = &StatusServerHandler;
        config.userData = const_cast<void*>(static_cast<const void*>(searchPaths));
        RunServer(ctx, config);
    });
}

} // end namespace coop::http
} // end namespace coop
