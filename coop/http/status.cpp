#include "status.h"
#include "server.h"
#include "connection.h"

#include <cstdint>
#include <string>

#include "coop/cooperator.h"
#include "coop/context.h"
#include "coop/detail/scheduler_state.h"
#include "coop/perf/counters.h"
#include "coop/perf/patch.h"

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
};

const char* StateString(SchedulerState state)
{
    switch (state)
    {
        case SchedulerState::RUNNING: return "running";
        case SchedulerState::YIELDED: return "yielded";
        case SchedulerState::BLOCKED: return "blocked";
    }
    return "unknown";
}

void SerializeContext(JsonWriter& w, Context* ctx)
{
    w.BeginObject();

    w.Key("name");
    w.String(ctx->GetName());

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

std::string GenerateStatusJson(Cooperator* co)
{
    std::string out;
    out.reserve(4096);
    JsonWriter w(out);

    w.BeginObject();

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
    return out;
}

void HandleStatus(ConnectionBase& conn)
{
    std::string body = GenerateStatusJson(conn.GetCooperator());
    conn.Send(200, "application/json", body);
}

std::string GeneratePerfJson(Cooperator* co)
{
    std::string out;
    out.reserve(1024);
    JsonWriter w(out);

    w.BeginObject();

    w.Key("mode");
    w.Int(COOP_PERF_MODE);

    w.Key("enabled");
    w.Bool(perf::IsEnabled());

    w.Key("probeCount");
    w.UInt(perf::ProbeCount());

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
    perf::Enable();
    conn.Send(200, "application/json", "{\"ok\":true}");
}

void HandlePerfDisable(ConnectionBase& conn)
{
    perf::Disable();
    conn.Send(200, "application/json", "{\"ok\":true}");
}

Route s_statusRoutes[] = {
    {"/api/status",       HandleStatus},
    {"/api/perf",         HandlePerf},
    {"/api/perf/enable",  HandlePerfEnable},
    {"/api/perf/disable", HandlePerfDisable},
};

static constexpr int STATUS_ROUTE_COUNT = sizeof(s_statusRoutes) / sizeof(s_statusRoutes[0]);

} // end anonymous namespace

void SpawnStatusServer(Cooperator* co, int port,
                       const char* const* searchPaths /* = nullptr */)
{
    co->Spawn([=](Context* ctx)
    {
        RunServer(ctx, port, s_statusRoutes, STATUS_ROUTE_COUNT, "HttpStatusServer", searchPaths);
    });
}

} // end namespace coop::http
} // end namespace coop
