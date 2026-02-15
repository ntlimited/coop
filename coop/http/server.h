#pragma once

#include <cstdint>
#include <string>

namespace coop
{

struct Cooperator;
struct Context;

namespace http
{

struct Route
{
    const char* path;
    const char* contentType;
    std::string (*handler)(Cooperator*);
};

// Run an HTTP server on the given port with the provided route table. Binds, listens, and accepts
// connections in a loop, launching a handler context per client. GET-only, HTTP/1.0-style (close
// after response).
//
void RunServer(
    Context* ctx,
    int port,
    const Route* routes,
    int routeCount,
    const char* name = "HttpServer",
    const char* const* searchPaths = nullptr);

} // end namespace coop::http
} // end namespace coop
