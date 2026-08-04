#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/thread.h"
#include "coop/http/server.h"
#include "coop/http/connection.h"
#include "coop/http/status.h"
#include "coop/io/ssl/context.h"

using namespace coop;

void HandlePlaintext(http::ConnectionBase& conn)
{
    conn.Send(200, "text/plain", "Hello, World!", 13);
}

void HandleJson(http::ConnectionBase& conn)
{
    conn.Send(200, "application/json", R"({"message":"Hello, World!"})", 27);
}

// This benchmark owns its own routing — coop no longer does path matching. A tiny app route table,
// then (optionally) the built-in status API via http::StatusDispatch, then static files, then 404.
//
struct AppRoute { const char* path; void (*handler)(http::ConnectionBase&); };

static const AppRoute s_appRoutes[] = {
    {"/plaintext", HandlePlaintext},
    {"/json", HandleJson},
};

static bool s_status = false;
static const char* const* s_searchPaths = nullptr;

void BenchDispatch(http::ConnectionBase& conn, void*)
{
    std::string_view path = conn.GetRequestLine()->path;

    for (auto& route : s_appRoutes)
    {
        if (path == route.path) { route.handler(conn); return; }
    }
    if (s_status && http::StatusDispatch(conn, path)) return;
    if (s_searchPaths && http::ServeFile(conn, path, s_searchPaths)) return;

    conn.Send(404, "text/plain", "Not Found\n");
}

struct TlsArgs
{
    int port;
    io::ssl::Context* sslCtx;
};

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);

    int port = 8080;
    bool status = false;
    bool sqpoll = false;
    bool shareSqpoll = false;
    bool tls = false;
    int workers = 1;
    const char* certPath = nullptr;
    const char* keyPath = nullptr;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--sqpoll") == 0) sqpoll = true;
        else if (strcmp(argv[i], "--share-sqpoll") == 0) { sqpoll = true; shareSqpoll = true; }
        else if (strcmp(argv[i], "--tls") == 0) tls = true;
        else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) certPath = argv[++i];
        else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) keyPath = argv[++i];
        else if (strcmp(argv[i], "--status") == 0) status = true;
        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) workers = atoi(argv[++i]);
        else port = atoi(argv[i]);
    }

    if (workers < 1) workers = 1;

    CooperatorConfiguration config = s_defaultCooperatorConfiguration;
    if (sqpoll)
    {
        config.uring.sqpoll = true;
        config.uring.coopTaskrun = false;
        config.uring.entries = 1024;
    }

    s_status = status;

    static const char* staticPaths[] = {"static", nullptr};
    s_searchPaths = status ? staticPaths : nullptr;

    fprintf(stderr, "Starting %d worker%s on port %d\n", workers, workers > 1 ? "s" : "", port);

    // Spawn N cooperators, each with its own SO_REUSEPORT accept loop.
    //
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
        snprintf(nameBuf, sizeof(nameBuf), "worker-%d", w);

        CooperatorConfiguration wConfig = config;
        wConfig.SetName(nameBuf);

        // For shared SQPOLL: workers 1+ attach to worker 0's kernel polling thread.
        // We need worker 0's uring to be initialized first.
        //
        if (shareSqpoll && w > 0)
        {
            // Wait for worker 0's uring to be initialized (ring_fd becomes valid)
            //
            auto* firstUring = pool[0].cooperator->GetUring();
            while (firstUring->RingFd() == 0)
            {
                usleep(100);
            }
            wConfig.uring.attachSqFd = firstUring->RingFd();
        }

        auto* co = new Cooperator(wConfig);
        auto* th = new Thread(co);
        th->PinToCore(w);
        pool.push_back({co, th});

        if (tls)
        {
            if (!certPath) certPath = "test_cert.pem";
            if (!keyPath) keyPath = "test_key.pem";

            SpawnConfiguration tlsConfig = {.priority = 0, .stackSize = 65536};
            co->Submit([](Context* ctx, void* arg) {
                int port = static_cast<int>(reinterpret_cast<intptr_t>(arg));

                char certBuf[8192], keyBuf[8192];
                FILE* f = fopen("test_cert.pem", "r");
                if (!f) { fprintf(stderr, "Failed to open cert file\n"); return; }
                size_t certLen = fread(certBuf, 1, sizeof(certBuf), f);
                fclose(f);

                f = fopen("test_key.pem", "r");
                if (!f) { fprintf(stderr, "Failed to open key file\n"); return; }
                size_t keyLen = fread(keyBuf, 1, sizeof(keyBuf), f);
                fclose(f);

                io::ssl::Context sslCtx(io::ssl::Mode::Server);
                sslCtx.LoadCertificate(certBuf, certLen);
                sslCtx.LoadPrivateKey(keyBuf, keyLen);
                sslCtx.EnableKTLS();

                http::ServerConfiguration cfg;
                cfg.port = port;
                cfg.name = "BenchTlsServer";
                cfg.handler = &BenchDispatch;
                cfg.timeout = std::chrono::seconds(0);
                http::RunTlsServer(ctx, cfg, sslCtx);
            }, reinterpret_cast<void*>(static_cast<intptr_t>(port)), tlsConfig);
        }
        else
        {
            co->Submit([](Context* ctx, void* arg) {
                int port = static_cast<int>(reinterpret_cast<intptr_t>(arg));
                http::ServerConfiguration cfg;
                cfg.port = port;
                cfg.name = "BenchServer";
                cfg.handler = &BenchDispatch;
                cfg.timeout = std::chrono::seconds(0);
                http::RunServer(ctx, cfg);
            }, reinterpret_cast<void*>(static_cast<intptr_t>(port)));
        }
    }

    // Block forever — cooperators run until killed. Thread dtors would join but we used
    // raw pointers, so just join the first thread manually.
    //
    pool[0].thread->m_thread.join();

    return 0;
}
