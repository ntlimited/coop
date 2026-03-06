#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "coop/http/transport.h"
#include "coop/io/connect.h"
#include "coop/io/io.h"

using namespace coop;

static std::atomic<int64_t> g_requests{0};
static std::atomic<int64_t> g_errors{0};
static std::atomic<bool> g_running{true};

// A single HTTP client context: connects, then hammers GET in a loop.
//
struct HttpClientWorker : Launchable
{
    HttpClientWorker(Context* ctx, const char* host, int port, const char* path)
    : Launchable(ctx)
    , m_host(host)
    , m_port(port)
    , m_path(path)
    {
        ctx->SetName("HttpClient");
        ctx->Detach();
    }

    virtual void Launch() final
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) return;

        io::Descriptor desc(fd);

        int ret = io::Connect(desc, m_host, m_port);
        if (ret < 0)
        {
            g_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Disable Nagle — we send small requests, don't want them delayed
        //
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        using Client = http::ClientConnection<http::PlaintextTransport>;
        http::PlaintextTransport transport(desc);
        auto conn = GetContext()->Allocate<Client>(
            Client::ExtraBytes(), transport, m_host);

        int count = 0;
        while (!GetContext()->IsKilled() && g_running.load(std::memory_order_relaxed))
        {
            if (!conn->Get(m_path))
            {
                fprintf(stderr, "  [client] Get failed after %d requests\n", count);
                g_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            auto* resp = conn->GetResponseLine();
            if (!resp)
            {
                fprintf(stderr, "  [client] GetResponseLine null after %d requests\n", count);
                g_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            conn->SkipBody();
            count++;
            g_requests.fetch_add(1, std::memory_order_relaxed);

            if (!conn->KeepAlive())
            {
                fprintf(stderr, "  [client] not keep-alive after %d requests\n", count);
                return;
            }
            conn->Reset();
        }
    }

    const char* m_host;
    int         m_port;
    const char* m_path;
};

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);

    const char* host = "127.0.0.1";
    int port = 8080;
    int connections = 64;
    int duration = 10;
    int workers = 1;
    const char* path = "/plaintext";

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) connections = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) workers = atoi(argv[++i]);
        else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) path = argv[++i];
        else if (argv[i][0] != '-') port = atoi(argv[i]);
    }

    if (workers < 1) workers = 1;

    int connsPerWorker = connections / workers;
    if (connsPerWorker < 1) connsPerWorker = 1;

    fprintf(stderr, "bench_client: %d workers, %d connections each (%d total), "
            "%ds duration, target %s:%d%s\n",
            workers, connsPerWorker, workers * connsPerWorker, duration, host, port, path);

    struct Worker
    {
        Cooperator* cooperator;
        Thread* thread;
    };

    std::vector<Worker> pool;
    pool.reserve(workers);

    for (int w = 0; w < workers; w++)
    {
        char nameBuf[32];
        snprintf(nameBuf, sizeof(nameBuf), "client-%d", w);

        CooperatorConfiguration config = s_defaultCooperatorConfiguration;
        config.SetName(nameBuf);

        auto* co = new Cooperator(config);
        auto* th = new Thread(co);
        th->PinToCore(w + 1);
        pool.push_back({co, th});

        co->Submit([=](Context* ctx) {
            for (int c = 0; c < connsPerWorker; c++)
            {
                static constexpr SpawnConfiguration clientConfig =
                    {.priority = 0, .stackSize = 16384};
                ctx->GetCooperator()->Launch<HttpClientWorker>(
                    clientConfig, host, port, path);
            }
        });
    }

    // Report stats every second
    //
    int64_t prevRequests = 0;
    int64_t prevErrors = 0;

    for (int s = 0; s < duration; s++)
    {
        sleep(1);

        int64_t curRequests = g_requests.load(std::memory_order_relaxed);
        int64_t curErrors = g_errors.load(std::memory_order_relaxed);
        int64_t rps = curRequests - prevRequests;
        int64_t eps = curErrors - prevErrors;

        fprintf(stderr, "  [%2d/%ds]  %7ld req/s", s + 1, duration, rps);
        if (eps > 0) fprintf(stderr, "  (%ld errors)", eps);
        fprintf(stderr, "  total: %ld\n", curRequests);

        prevRequests = curRequests;
        prevErrors = curErrors;
    }

    g_running.store(false, std::memory_order_relaxed);

    int64_t totalRequests = g_requests.load(std::memory_order_relaxed);
    int64_t totalErrors = g_errors.load(std::memory_order_relaxed);

    fprintf(stderr, "\n--- Results ---\n");
    fprintf(stderr, "  Total requests: %ld\n", totalRequests);
    fprintf(stderr, "  Total errors:   %ld\n", totalErrors);
    fprintf(stderr, "  Avg req/s:      %ld\n", totalRequests / duration);
    fprintf(stderr, "  Duration:       %ds\n", duration);
    fprintf(stderr, "  Connections:    %d\n", workers * connsPerWorker);

    // Shutdown cooperators
    //
    for (auto& w : pool)
    {
        w.cooperator->Shutdown();
    }
    for (auto& w : pool)
    {
        delete w.thread;
        delete w.cooperator;
    }

    return 0;
}
