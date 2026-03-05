#include <csignal>
#include <cstdlib>
#include <cstring>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/thread.h"
#include "coop/http/server.h"
#include "coop/http/connection.h"

using namespace coop;

void HandlePlaintext(http::Connection& conn)
{
    conn.Send(200, "text/plain", "Hello, World!", 13);
}

void HandleJson(http::Connection& conn)
{
    conn.Send(200, "application/json", R"({"message":"Hello, World!"})", 27);
}

static const http::Route s_routes[] = {
    {"/plaintext", HandlePlaintext},
    {"/json", HandleJson},
};

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);

    int port = 8080;
    bool sqpoll = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--sqpoll") == 0) sqpoll = true;
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

    cooperator.Submit([](Context* ctx, void* arg) {
        int port = static_cast<int>(reinterpret_cast<intptr_t>(arg));
        http::RunServer(ctx, port, s_routes, 2, "BenchServer", nullptr,
                        std::chrono::seconds(0));
    }, reinterpret_cast<void*>(static_cast<intptr_t>(port)));

    return 0;
}
