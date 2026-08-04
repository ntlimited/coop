// Pbuf-vs-classic HTTP recv path under keep-alive load.
//
// The pbuf mode (ServerConfiguration::pbufRecv) serves a connection's lifetime from one
// armed multishot recv, parsing request heads zero-copy from kernel-selected pool
// chunks; classic mode issues a one-shot recv per request into the connection's own
// buffer. This harness measures what that trade is worth on the keep-alive
// request/response shape pbuf mode targets.
//
// Shape: one server cooperator runs RunServer (identical routes/config apart from
// pbufRecv); driver threads hold keep-alive connections in a closed loop — one in-flight
// request per connection — for a fixed window. Phases alternate classic/pbuf so ambient
// load on a shared box skews both equally. Metrics: aggregate requests/sec and
// user+sys CPU seconds per 100k requests (the load-robust number).
//
// Run in release, pinned: build/release/bin/bench_pbuf_http [connections] [seconds]

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/thread.h"

#include "coop/http/connection.h"
#include "coop/http/server.h"

namespace
{

constexpr int kDriverThreads = 4;

void OkHandler(coop::http::ConnectionBase& conn, void*)
{
    conn.Send(200, "text/plain", "OK");
}

int ConnectTo(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

// One blocking request/response round trip. Returns false on any socket error.
//
bool RoundTrip(int fd, const char* request, size_t reqLen, size_t respLen)
{
    size_t sent = 0;
    while (sent < reqLen)
    {
        ssize_t w = ::send(fd, request + sent, reqLen - sent, MSG_NOSIGNAL);
        if (w <= 0) return false;
        sent += static_cast<size_t>(w);
    }

    char buf[512];
    size_t got = 0;
    while (got < respLen)
    {
        ssize_t r = ::recv(fd, buf, sizeof(buf) < respLen - got ? sizeof(buf)
                                                                : respLen - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

double CpuSeconds()
{
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    auto tv = [](const timeval& t) { return t.tv_sec + t.tv_usec / 1e6; };
    return tv(ru.ru_utime) + tv(ru.ru_stime);
}

struct PhaseResult
{
    uint64_t requests = 0;
    double   wallSec = 0.0;
    double   cpuSec = 0.0;
};

// Run one measurement phase against an already-listening server port.
//
PhaseResult RunPhase(int port, int connections, int seconds)
{
    const char* request = "GET /ok HTTP/1.1\r\nHost: bench\r\n\r\n";
    size_t reqLen = strlen(request);

    // Learn the exact response size once — every response is byte-identical
    //
    int probe = ConnectTo(port);
    if (probe < 0)
    {
        fprintf(stderr, "probe connect failed: %s\n", strerror(errno));
        exit(1);
    }
    size_t respLen = 0;
    {
        size_t sent = 0;
        while (sent < reqLen)
        {
            ssize_t w = ::send(probe, request + sent, reqLen - sent, MSG_NOSIGNAL);
            if (w <= 0) { exit(1); }
            sent += static_cast<size_t>(w);
        }
        char buf[512];
        // The whole small response arrives promptly; read once after a short settle
        //
        for (;;)
        {
            ssize_t r = ::recv(probe, buf + respLen, sizeof(buf) - respLen, 0);
            if (r <= 0) break;
            respLen += static_cast<size_t>(r);
            if (respLen >= 4 &&
                memmem(buf, respLen, "\r\n\r\nOK", 6) != nullptr)
            {
                break;
            }
        }
    }
    close(probe);
    if (respLen == 0)
    {
        fprintf(stderr, "probe got no response\n");
        exit(1);
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total{0};

    double cpu0 = CpuSeconds();
    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> drivers;
    int per = connections / kDriverThreads;
    for (int d = 0; d < kDriverThreads; d++)
    {
        drivers.emplace_back([&, per]
        {
            std::vector<int> fds;
            for (int i = 0; i < per; i++)
            {
                int fd = ConnectTo(port);
                if (fd >= 0) fds.push_back(fd);
            }

            uint64_t mine = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                for (int fd : fds)
                {
                    if (RoundTrip(fd, request, reqLen, respLen))
                    {
                        mine++;
                    }
                }
            }
            total.fetch_add(mine, std::memory_order_relaxed);

            for (int fd : fds)
            {
                close(fd);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop.store(true);
    for (auto& t : drivers)
    {
        t.join();
    }

    PhaseResult result;
    result.wallSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    result.cpuSec = CpuSeconds() - cpu0;
    result.requests = total.load();
    return result;
}

} // end anonymous namespace

int main(int argc, char** argv)
{
    int connections = argc > 1 ? atoi(argv[1]) : 64;
    int seconds = argc > 2 ? atoi(argv[2]) : 3;
    const char* only = argc > 3 ? argv[3] : "both";
    int modeLo = strcmp(only, "pbuf") == 0 ? 1 : 0;
    int modeHi = strcmp(only, "classic") == 0 ? 0 : 1;
    constexpr int kRounds = 2;

    int basePort = 42000 + (getpid() % 10000);

    // One server cooperator, buffer ring configured, running BOTH servers — the classic
    // one simply never touches the ring, so the memory/registration baseline is shared
    // and only the recv path differs.
    //
    coop::CooperatorConfiguration cfg;
    cfg.uring.bufferRingEntries = 256;
    cfg.uring.bufferRingBufSize = 2048;
    coop::Cooperator cooperator(cfg);
    coop::Thread thread(&cooperator);

    for (int mode = 0; mode < 2; mode++)
    {
        int port = basePort + mode;
        cooperator.Submit([port, mode](coop::Context* ctx)
        {
            coop::http::ServerConfiguration config;
            config.port = port;
            config.pbufRecv = (mode == 1);
            config.name = mode == 1 ? "PbufServer" : "ClassicServer";
            config.handler = &OkHandler;
            coop::http::RunServer(ctx, config);
        }, {.priority = 0, .stackSize = 65536});
    }

    // Let the servers bind
    //
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    PhaseResult results[2];
    for (int round = 0; round < kRounds; round++)
    {
        for (int mode = modeLo; mode <= modeHi; mode++)
        {
            PhaseResult r = RunPhase(basePort + mode, connections, seconds);
            results[mode].requests += r.requests;
            results[mode].wallSec += r.wallSec;
            results[mode].cpuSec += r.cpuSec;
        }
    }

    printf("\nkeep-alive HTTP recv path: classic vs pbuf "
           "(%d connections, %d drivers, %dx%ds interleaved)\n\n",
           connections, kDriverThreads, kRounds, seconds);
    printf("%-8s %14s %20s\n", "mode", "req/s", "cpu-sec/100k-req");
    const char* names[2] = { "classic", "pbuf" };
    for (int mode = modeLo; mode <= modeHi; mode++)
    {
        const PhaseResult& r = results[mode];
        printf("%-8s %14.0f %20.3f\n", names[mode],
               r.requests / r.wallSec,
               r.requests ? r.cpuSec / (r.requests / 100000.0) : 0.0);
    }
    printf("\ncpu-sec includes the in-process driver threads (identical work per mode).\n");

    cooperator.Shutdown();
    return 0;
}
