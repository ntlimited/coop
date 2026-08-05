// bench_curl — curl-multi-under-coop vs the native coop HTTP client, over HTTP/1.1 keep-alive.
//
// One process: a coop HTTP server cooperator, and a client cooperator running N concurrent workers
// of the selected engine against it over loopback. Both engines hammer GET on persistent
// connections and count completed requests; the point is a like-for-like req/s comparison of the
// curl bridge (examples/curl_coop.h) against coop's own client on the same substrate.
//
//   ./bench_curl --engine native -c 64 -d 10
//   ./bench_curl --engine curl   -c 64 -d 10
//
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/launchable.h"
#include "coop/thread.h"
#include "coop/alloc.h"
#include "coop/http/client.h"
#include "coop/http/server.h"
#include "coop/http/connection.h"
#include "coop/http/transport.h"
#include "coop/io/connect.h"
#include "coop/io/io.h"

#include "../examples/curl_coop.h"

using namespace coop;

static std::atomic<int64_t> g_requests{0};
static std::atomic<int64_t> g_errors{0};
static std::atomic<bool>    g_running{true};

// ---- server ----

static void OkHandler(http::ConnectionBase& conn, void*)
{
    conn.Send(200, "text/plain", "Hello, World!");
}

// ---- native coop client worker ----

struct NativeWorker : Launchable
{
    NativeWorker(Context* ctx, const char* host, int port, const char* path)
    : Launchable(ctx), m_host(host), m_port(port), m_path(path)
    {
        ctx->SetName("native-client");
        ctx->Detach();
    }

    virtual void Launch() final
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) { g_errors.fetch_add(1); return; }

        io::Descriptor desc(fd);
        if (io::Connect(desc, m_host, m_port) < 0) { g_errors.fetch_add(1); return; }

        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        using Client = http::ClientConnection<http::PlaintextTransport>;
        http::PlaintextTransport transport(desc);
        auto conn = GetContext()->Allocate<Client>(Client::ExtraBytes(), transport, m_host);

        while (!GetContext()->IsKilled() && g_running.load(std::memory_order_relaxed))
        {
            if (!conn->Get(m_path))            { g_errors.fetch_add(1); return; }
            if (!conn->GetResponseLine())      { g_errors.fetch_add(1); return; }
            conn->SkipBody();
            g_requests.fetch_add(1, std::memory_order_relaxed);
            if (!conn->KeepAlive())            return;
            conn->Reset();
        }
    }

    const char* m_host; int m_port; const char* m_path;
};

// ---- curl-driven client worker ----

static size_t DiscardBody(char*, size_t sz, size_t n, void*) { return sz * n; }

static void CurlWorker(Context* ctx, curlcoop::Driver* driver, const char* url)
{
    ctx->SetName("curl-client");
    ctx->Detach();

    CURL* easy = curl_easy_init();
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &DiscardBody);
    curl_easy_setopt(easy, CURLOPT_TCP_NODELAY, 1L);
    // HTTP/1.1 keep-alive: curl's connection cache reuses the socket across Perform calls.
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    while (!ctx->IsKilled() && g_running.load(std::memory_order_relaxed))
    {
        long status = 0;
        CURLcode rc = driver->Perform(ctx, easy, &status);
        if (rc != CURLE_OK || status != 200) { g_errors.fetch_add(1); break; }
        g_requests.fetch_add(1, std::memory_order_relaxed);
    }

    curl_easy_cleanup(easy);
}

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);

    const char* engine = "native";
    int port = 18080;
    int connections = 64;
    int duration = 10;
    const char* path = "/plaintext";

    for (int i = 1; i < argc; i++)
    {
        if      (!strcmp(argv[i], "--engine") && i + 1 < argc) engine = argv[++i];
        else if (!strcmp(argv[i], "--port")   && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c")       && i + 1 < argc) connections = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d")       && i + 1 < argc) duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--path")   && i + 1 < argc) path = argv[++i];
    }
    bool useCurl = !strcmp(engine, "curl");

    if (useCurl) curl_global_init(CURL_GLOBAL_DEFAULT);

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", port, path);

    fprintf(stderr, "bench_curl: engine=%s connections=%d duration=%ds target=%s\n",
            engine, connections, duration, url);

    // Server on its own cooperator/thread.
    //
    CooperatorConfiguration serverCfg = s_defaultCooperatorConfiguration;
    serverCfg.SetName("server");
    Cooperator serverCo(serverCfg);
    Thread serverThread(&serverCo);
    serverThread.PinToCore(0);
    serverCo.Submit([port](Context* ctx)
    {
        http::ServerConfiguration config;
        config.port = port;
        config.name = "BenchServer";
        config.handler = &OkHandler;
        http::RunServer(ctx, config);
    }, {.priority = 0, .stackSize = 65536});

    usleep(200 * 1000);   // let the listener bind

    // Client on its own cooperator/thread.
    //
    CooperatorConfiguration clientCfg = s_defaultCooperatorConfiguration;
    clientCfg.SetName("client");
    Cooperator clientCo(clientCfg);
    Thread clientThread(&clientCo);
    clientThread.PinToCore(1);

    clientCo.Submit([&, useCurl](Context* ctx)
    {
        if (useCurl)
        {
            // One driver shared by all curl workers on this cooperator (leaked at exit — fine).
            //
            auto* driver = new curlcoop::Driver(&clientCo);
            for (int c = 0; c < connections; c++)
            {
                clientCo.Spawn({.priority = 0, .stackSize = 32768},
                               [driver, u = std::string(url)](Context* w)
                               { CurlWorker(w, driver, u.c_str()); });
            }
        }
        else
        {
            for (int c = 0; c < connections; c++)
            {
                static constexpr SpawnConfiguration wc = {.priority = 0, .stackSize = 16384};
                clientCo.Launch<NativeWorker>(wc, "127.0.0.1", port, path);
            }
        }
    });

    // Report req/s each second.
    //
    int64_t prev = 0;
    for (int s = 0; s < duration; s++)
    {
        sleep(1);
        int64_t cur = g_requests.load(std::memory_order_relaxed);
        fprintf(stderr, "  [%2d/%ds]  %8ld req/s  total=%ld\n",
                s + 1, duration, cur - prev, cur);
        prev = cur;
    }

    g_running.store(false, std::memory_order_relaxed);
    usleep(300 * 1000);   // let in-flight requests drain

    int64_t total = g_requests.load(std::memory_order_relaxed);
    int64_t errs  = g_errors.load(std::memory_order_relaxed);
    fprintf(stderr, "\n--- %s ---\n", engine);
    fprintf(stderr, "  total requests: %ld\n", total);
    fprintf(stderr, "  errors:         %ld\n", errs);
    fprintf(stderr, "  avg req/s:      %ld\n", total / duration);

    clientCo.Shutdown();
    serverCo.Shutdown();
    return 0;
}
