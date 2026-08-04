#pragma once

#include <string_view>

namespace coop
{

struct Cooperator;
struct Context;

namespace http
{

struct ConnectionBase;

// Handle the built-in status/perf/sampler/epoch JSON API if `path` names one of its endpoints
// (/api/status, /api/perf[...], /api/sampler[...], /api/cooperators[...], /api/epoch[...]). Returns
// true if it wrote a response, false if the path is none of them. This is a composable handler, not
// a route table: an application drops it into its own RequestHandler
// (`if (http::StatusDispatch(conn, path)) return;`) to expose the dashboard API alongside its own
// endpoints, and coop keeps no matching machinery of its own.
//
bool StatusDispatch(ConnectionBase& conn, std::string_view path);

// Spawn a standalone HTTP status server on the given port: a RequestHandler that runs StatusDispatch
// and then falls back to static files under `searchPaths` (null-terminated; the dashboard's
// index.html etc.), else 404. Call from within a coop context.
//
void SpawnStatusServer(Cooperator* co, int port,
                       const char* const* searchPaths = nullptr);

} // end namespace coop::http
} // end namespace coop
