#pragma once

#include <cstdint>

#include "coop/time/interval.h"

namespace coop
{

struct Cooperator;
struct Context;

namespace http
{

struct Connection;

struct Route
{
    const char* path;
    void (*handler)(Connection&);
};

// Run an HTTP server on the given port with the provided route table. Binds, listens, and accepts
// connections in a loop, launching a handler context per client. Connection: close after response.
//
void RunServer(
    Context* ctx,
    int port,
    const Route* routes,
    int routeCount,
    const char* name = "HttpServer",
    const char* const* searchPaths = nullptr,
    time::Interval timeout = std::chrono::seconds(30));

} // end namespace coop::http
} // end namespace coop
