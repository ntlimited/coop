#pragma once

namespace coop
{

struct Cooperator;
struct Context;

namespace http
{

// Spawn an HTTP status server on the given port. Provides a JSON API at GET /api/status with the
// cooperator's context tree. Static files (including the dashboard at index.html) are served from
// the search paths (null-terminated array). Call from within a coop context.
//
void SpawnStatusServer(Cooperator* co, int port,
                       const char* const* searchPaths = nullptr);

} // end namespace coop::http
} // end namespace coop
