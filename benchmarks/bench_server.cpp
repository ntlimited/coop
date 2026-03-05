#include <csignal>
#include <cstdlib>
#include <cstring>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/thread.h"
#include "coop/http/server.h"
#include "coop/http/connection.h"
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

static const http::Route s_routes[] = {
    {"/plaintext", HandlePlaintext},
    {"/json", HandleJson},
};

struct TlsArgs
{
    int port;
    io::ssl::Context* sslCtx;
};

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);

    int port = 8080;
    bool sqpoll = false;
    bool tls = false;
    const char* certPath = nullptr;
    const char* keyPath = nullptr;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--sqpoll") == 0) sqpoll = true;
        else if (strcmp(argv[i], "--tls") == 0) tls = true;
        else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) certPath = argv[++i];
        else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) keyPath = argv[++i];
        else port = atoi(argv[i]);
    }

    CooperatorConfiguration config = s_defaultCooperatorConfiguration;
    if (sqpoll)
    {
        config.uring.sqpoll = true;
        config.uring.coopTaskrun = false;
        config.uring.entries = 1024;
    }

    Cooperator cooperator(config);
    Thread t(&cooperator);

    if (tls)
    {
        if (!certPath) certPath = "test_cert.pem";
        if (!keyPath) keyPath = "test_key.pem";

        cooperator.Submit([](Context* ctx, void* arg) {
            auto* a = static_cast<TlsArgs*>(arg);

            // Read cert and key files
            //
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

            http::RunTlsServer(ctx, a->port, s_routes, 2, sslCtx, "BenchTlsServer",
                               nullptr, std::chrono::seconds(0));
        }, new TlsArgs{port, nullptr});
    }
    else
    {
        cooperator.Submit([](Context* ctx, void* arg) {
            int port = static_cast<int>(reinterpret_cast<intptr_t>(arg));
            http::RunServer(ctx, port, s_routes, 2, "BenchServer", nullptr,
                            std::chrono::seconds(0));
        }, reinterpret_cast<void*>(static_cast<intptr_t>(port)));
    }

    return 0;
}
