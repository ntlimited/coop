#pragma once

namespace coop
{

struct Cooperator;
struct Context;

namespace http
{

struct Route;

// Return the built-in status/perf API route table (/api/status, /api/perf, /api/perf/enable,
// /api/perf/disable). These can be appended to an application's own route table so that the
// status dashboard shares the same port as the application server.
//
const Route* StatusRoutes();
int StatusRouteCount();

// Spawn an HTTP status server on the given port. Provides a JSON API at GET /api/status with the
// cooperator's context tree. Static files (including the dashboard at index.html) are served from
// the search paths (null-terminated array). Call from within a coop context.
//
void SpawnStatusServer(Cooperator* co, int port,
                       const char* const* searchPaths = nullptr);

} // end namespace coop::http
} // end namespace coop
