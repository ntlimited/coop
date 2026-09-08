// Isolate HTTP parsing from socket/kernel cost. The compile-time transport copies a
// fixed response into the parser's receive window; no ring or scheduler is initialized.
// Run in release mode, pinned, and compare identical workloads/compiler configurations.
// This is a parser CPU measurement, not end-to-end client throughput.
//
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string_view>

#include "coop/http/client.h"
#include "coop/http/detail/client_impl.hpp"
#include "coop/http/detail/server_impl.hpp"
#include "coop/http/transport.h"
#include "coop/io/uring.h"

namespace
{
struct BufferedTransport
{
    static constexpr bool kSpliceable = false;
    std::string_view response;
    size_t maxRead;
    size_t position{0};

    int Recv(void* destination, size_t capacity, int, coop::time::Interval)
    {
        size_t size = std::min({capacity, maxRead, response.size() - position});
        memcpy(destination, response.data() + position, size);
        position += size;
        return static_cast<int>(size);
    }
    int SendAll(const void*, size_t size) { return static_cast<int>(size); }
    int SendfileAll(int, off_t, size_t) { return -ENOTSUP; }
};

void ParseResponse(benchmark::State& state)
{
    constexpr std::string_view fixed =
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: 13\r\nConnection: keep-alive\r\n"
        "X-Origin: test-origin.example.net\r\n\r\nHello, World!";
    constexpr std::string_view chunked =
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n"
        "d\r\nHello, World!\r\n0\r\n\r\n";
    std::string_view response = state.range(0) ? chunked : fixed;
    using Client = coop::http::ClientConnection<BufferedTransport>;
    alignas(Client) char storage[sizeof(Client) + Client::ExtraBytes()];
    auto* client = new (storage) Client(
        BufferedTransport{response, static_cast<size_t>(state.range(1))}, "example.test");
    for (auto _ : state)
    {
        while (auto body = client->NextBody())
        {
            benchmark::DoNotOptimize(body->data);
            benchmark::DoNotOptimize(body->size);
        }
        if (!client->Reusable() || !client->Reset())
        {
            state.SkipWithError("response did not complete");
            break;
        }
        client->m_transport.position = 0;
    }
    state.SetBytesProcessed(state.iterations() * response.size());
    state.counters["connection_bytes"] = sizeof(Client);
    client->~Client();
}

BENCHMARK(ParseResponse)->Args({0, 4096})->Args({1, 4096})->Args({0, 7})->Args({1, 7});

struct BufferedServerTransport : BufferedTransport
{
    coop::io::Descriptor* descriptor;
    coop::io::Descriptor& Descriptor() { return *descriptor; }
};

void ParseRequest(benchmark::State& state)
{
    constexpr std::string_view fixed =
        "POST /upload HTTP/1.1\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: 13\r\nConnection: keep-alive\r\n"
        "X-Origin: test-origin.example.net\r\n\r\nHello, World!";
    constexpr std::string_view chunked =
        "POST /upload HTTP/1.1\r\nContent-Type: application/octet-stream\r\n"
        "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n"
        "d\r\nHello, World!\r\n0\r\n\r\n";
    std::string_view request = state.range(0) ? chunked : fixed;
    // The server exposes its descriptor to handlers; parsing itself needs no kernel
    // operations. This borrowed invalid descriptor belongs to an uninitialized ring.
    coop::io::Uring ring;
    coop::io::Descriptor descriptor(coop::io::borrowed, -1, &ring);
    using Server = coop::http::Connection<BufferedServerTransport>;
    alignas(Server) char storage[sizeof(Server) + Server::ExtraBytes()];
    auto* server = new (storage) Server(
        BufferedServerTransport{{request, static_cast<size_t>(state.range(1))}, &descriptor},
        nullptr, nullptr);
    for (auto _ : state)
    {
        while (auto body = server->NextBody())
        {
            benchmark::DoNotOptimize(body->data);
            benchmark::DoNotOptimize(body->size);
        }
        if (!server->Reset())
        {
            state.SkipWithError("request did not complete on a reusable connection");
            break;
        }
        server->m_transport.position = 0;
    }
    state.SetBytesProcessed(state.iterations() * request.size());
    state.counters["connection_bytes"] = sizeof(Server);
    state.counters["native_connection_bytes"] =
        sizeof(coop::http::Connection<coop::http::PlaintextTransport>);
    server->~Server();
}

BENCHMARK(ParseRequest)->Args({0, 4096})->Args({1, 4096})->Args({0, 7})->Args({1, 7});
} // namespace

BENCHMARK_MAIN();
