#include <cassert>
#include <cstring>
#include <functional>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/thread.h"

#include "coop/io/descriptor.h"
#include "coop/io/read_file.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/io/ssl/ssl.h"

#include "coop/alloc.h"
#include "coop/http/connection.h"
#include "coop/http/transport.h"
#include "coop/http/tls_transport.h"

static constexpr size_t HTTP_BUF = coop::http::ConnectionBase::DEFAULT_BUFFER_SIZE;

using HttpConn = coop::http::Connection<coop::http::PlaintextTransport>;
using HttpTlsConn = coop::http::Connection<coop::http::TlsTransport>;

// ---------------------------------------------------------------------------
// Helper: run a benchmark body inside a cooperator
// ---------------------------------------------------------------------------

struct BenchmarkArgs
{
    benchmark::State* state;
    std::function<void(coop::Context*, benchmark::State&)>* fn;
};

static void RunBenchmark(benchmark::State& state,
    std::function<void(coop::Context*, benchmark::State&)> fn,
    coop::SpawnConfiguration config = coop::s_defaultConfiguration)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    BenchmarkArgs args;
    args.state = &state;
    args.fn = &fn;

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        auto* a = static_cast<BenchmarkArgs*>(arg);
        (*a->fn)(ctx, *a->state);
        ctx->GetCooperator()->Shutdown();
    }, &args, config);
}

// ---------------------------------------------------------------------------
// Transport helpers
// ---------------------------------------------------------------------------

static void MakeSocketPair(int fds[2])
{
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(ret == 0);
    (void)ret;
}

static void MakeTcpPair(int fds[2])
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    int ret = bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    assert(ret == 0);
    ret = listen(listen_fd, 1);
    assert(ret == 0);

    socklen_t len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), &len);

    fds[1] = socket(AF_INET, SOCK_STREAM, 0);
    assert(fds[1] >= 0);
    ret = connect(fds[1], reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    assert(ret == 0);

    fds[0] = accept(listen_fd, nullptr, nullptr);
    assert(fds[0] >= 0);
    close(listen_fd);

    // Disable Nagle on both ends for consistent latency
    //
    opt = 1;
    setsockopt(fds[0], IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    setsockopt(fds[1], IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    (void)ret;
}

// Drain pending data from a raw fd so the socket buffer doesn't fill up across iterations.
//
static void DrainFd(int fd)
{
    char buf[4096];
    while (true)
    {
        int n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) break;
    }
}

// ---------------------------------------------------------------------------
// Request templates
// ---------------------------------------------------------------------------

static const char REQ_MINIMAL[] =
    "GET / HTTP/1.1\r\n"
    "\r\n";

static const char REQ_WITH_HEADERS[] =
    "GET /path HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Accept: text/html\r\n"
    "User-Agent: bench/1.0\r\n"
    "Accept-Language: en-US\r\n"
    "Accept-Encoding: gzip, deflate\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char REQ_WITH_ARGS[] =
    "GET /search?q=hello&lang=en&page=1&limit=20 HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "\r\n";

static const char REQ_POST_BODY[] =
    "POST /data HTTP/1.1\r\n"
    "Content-Length: 13\r\n"
    "\r\n"
    "Hello, World!";

static const char RESP_BODY[] = "OK\n";

// Realistic GET: query args + typical browser headers + auth
//
static const char REQ_REALISTIC_GET[] =
    "GET /api/status?format=json&verbose=true HTTP/1.1\r\n"
    "Host: localhost:8080\r\n"
    "Accept: application/json\r\n"
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64) Gecko/20100101 Firefox/128.0\r\n"
    "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0\r\n"
    "Accept-Encoding: gzip, deflate, br\r\n"
    "Accept-Language: en-US,en;q=0.9\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

// Realistic POST: JSON body with typical headers
//
static const char REQ_REALISTIC_POST[] =
    "POST /api/events HTTP/1.1\r\n"
    "Host: localhost:8080\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 82\r\n"
    "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0\r\n"
    "Accept: application/json\r\n"
    "User-Agent: api-client/2.1\r\n"
    "\r\n"
    "{\"event\":\"page_view\",\"timestamp\":1709654400,\"user_id\":\"u_abc123\",\"page\":\"/dashboard\"}";

// JSON-ish response bodies of various sizes
//
static const char RESP_JSON_SMALL[] =
    "{\"status\":\"ok\",\"uptime\":86400,\"version\":\"1.2.3\"}";

// ~1KB response
//
static char RESP_1K[1024];

// ~4KB response
//
static char RESP_4K[4096];

static void InitResponseBodies()
{
    memset(RESP_1K, 'x', sizeof(RESP_1K) - 1);
    RESP_1K[0] = '{';
    RESP_1K[sizeof(RESP_1K) - 2] = '}';
    RESP_1K[sizeof(RESP_1K) - 1] = '\0';

    memset(RESP_4K, 'x', sizeof(RESP_4K) - 1);
    RESP_4K[0] = '{';
    RESP_4K[sizeof(RESP_4K) - 2] = '}';
    RESP_4K[sizeof(RESP_4K) - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Unix socket benchmarks — baseline for comparison
// ---------------------------------------------------------------------------

static void BM_Http_Unix_MinimalGet(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeSocketPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            coop::io::SendAll(client, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1);

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co, HTTP_BUF);
                auto* req = conn->GetRequestLine();
                assert(req);
                conn->Send(200, "text/plain", RESP_BODY, sizeof(RESP_BODY) - 1);
            }

            DrainFd(fds[1]);
        }
    });
}
BENCHMARK(BM_Http_Unix_MinimalGet);

static void BM_Http_Unix_SendResponse(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeSocketPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            state.PauseTiming();
            coop::io::SendAll(client, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1);
            state.ResumeTiming();

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co, HTTP_BUF);
                conn->GetRequestLine();
                conn->Send(200, "text/plain", RESP_BODY, sizeof(RESP_BODY) - 1);
            }

            state.PauseTiming();
            DrainFd(fds[1]);
            state.ResumeTiming();
        }
    });
}
BENCHMARK(BM_Http_Unix_SendResponse);

// ---------------------------------------------------------------------------
// TCP loopback benchmarks — same scenarios over TCP
// ---------------------------------------------------------------------------

static void BM_Http_Tcp_MinimalGet(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeTcpPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            coop::io::SendAll(client, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1);

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co, HTTP_BUF);
                auto* req = conn->GetRequestLine();
                assert(req);
                conn->Send(200, "text/plain", RESP_BODY, sizeof(RESP_BODY) - 1);
            }

            DrainFd(fds[1]);
        }
    });
}
BENCHMARK(BM_Http_Tcp_MinimalGet);

static void BM_Http_Tcp_SendResponse(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeTcpPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            state.PauseTiming();
            coop::io::SendAll(client, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1);
            state.ResumeTiming();

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co, HTTP_BUF);
                conn->GetRequestLine();
                conn->Send(200, "text/plain", RESP_BODY, sizeof(RESP_BODY) - 1);
            }

            state.PauseTiming();
            DrainFd(fds[1]);
            state.ResumeTiming();
        }
    });
}
BENCHMARK(BM_Http_Tcp_SendResponse);

static void BM_Http_Tcp_ChunkedResponse(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeTcpPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            state.PauseTiming();
            coop::io::SendAll(client, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1);
            state.ResumeTiming();

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co, HTTP_BUF);
                conn->GetRequestLine();
                conn->BeginChunked(200, "text/plain");
                conn->SendChunk("Hello", 5);
                conn->SendChunk(", ", 2);
                conn->EndChunked("World!", 6);
            }

            state.PauseTiming();
            DrainFd(fds[1]);
            state.ResumeTiming();
        }
    });
}
BENCHMARK(BM_Http_Tcp_ChunkedResponse);

// No-timeout variant — Connection uses plain Recv (1 SQE) instead of Recv with linked
// timeout (2 SQEs). Isolates the SubmitLinked overhead.
//
static void BM_Http_Tcp_MinimalGet_NoTimeout(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeTcpPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            coop::io::SendAll(client, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1);

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co,
                    HTTP_BUF, std::chrono::seconds(0));
                auto* req = conn->GetRequestLine();
                assert(req);
                conn->Send(200, "text/plain", RESP_BODY, sizeof(RESP_BODY) - 1);
            }

            DrainFd(fds[1]);
        }
    });
}
BENCHMARK(BM_Http_Tcp_MinimalGet_NoTimeout);

// ---------------------------------------------------------------------------
// Realistic handler scenarios (TCP)
// ---------------------------------------------------------------------------

// Realistic GET: parse request line + args, search headers for Authorization,
// send small JSON response.
//
static void BM_Http_Tcp_RealisticGet(benchmark::State& state)
{
    InitResponseBodies();

    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeTcpPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            coop::io::SendAll(client, REQ_REALISTIC_GET, sizeof(REQ_REALISTIC_GET) - 1);

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co, HTTP_BUF);
                auto* req = conn->GetRequestLine();
                assert(req);

                // Read query args
                //
                while (auto* name = conn->NextArgName())
                {
                    auto* val = conn->ReadArgValue();
                    benchmark::DoNotOptimize(val);
                }

                // Search for Authorization header
                //
                bool authorized = false;
                while (auto* name = conn->NextHeaderName())
                {
                    if (strncasecmp(name, "Authorization", 13) == 0)
                    {
                        auto* val = conn->ReadHeaderValue();
                        benchmark::DoNotOptimize(val);
                        authorized = true;
                    }
                    else
                    {
                        conn->SkipHeaderValue();
                    }
                }
                benchmark::DoNotOptimize(authorized);

                conn->Send(200, "application/json",
                    RESP_JSON_SMALL, sizeof(RESP_JSON_SMALL) - 1);
            }

            DrainFd(fds[1]);
        }
    });
}
BENCHMARK(BM_Http_Tcp_RealisticGet);

// Realistic POST: parse request line, read Content-Type + Authorization headers,
// consume body, send small JSON response.
//
static void BM_Http_Tcp_RealisticPost(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeTcpPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            coop::io::SendAll(client, REQ_REALISTIC_POST, sizeof(REQ_REALISTIC_POST) - 1);

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co, HTTP_BUF);
                auto* req = conn->GetRequestLine();
                assert(req);

                // Read specific headers
                //
                while (auto* name = conn->NextHeaderName())
                {
                    if (strncasecmp(name, "Content-Type", 12) == 0 ||
                        strncasecmp(name, "Authorization", 13) == 0)
                    {
                        auto* val = conn->ReadHeaderValue();
                        benchmark::DoNotOptimize(val);
                    }
                    else
                    {
                        conn->SkipHeaderValue();
                    }
                }

                // Consume body
                //
                while (auto* chunk = conn->ReadBody())
                {
                    benchmark::DoNotOptimize(chunk);
                }

                conn->Send(200, "application/json",
                    RESP_JSON_SMALL, sizeof(RESP_JSON_SMALL) - 1);
            }

            DrainFd(fds[1]);
        }
    });
}
BENCHMARK(BM_Http_Tcp_RealisticPost);

// Response size scaling: 1KB and 4KB bodies over TCP
//
static void BM_Http_Tcp_Response1K(benchmark::State& state)
{
    InitResponseBodies();

    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeTcpPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            coop::io::SendAll(client, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1);

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co, HTTP_BUF);
                conn->GetRequestLine();
                conn->Send(200, "application/json", RESP_1K, sizeof(RESP_1K) - 1);
            }

            DrainFd(fds[1]);
        }
    });
}
BENCHMARK(BM_Http_Tcp_Response1K);

static void BM_Http_Tcp_Response4K(benchmark::State& state)
{
    InitResponseBodies();

    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeTcpPair(fds);

        auto* co = ctx->GetCooperator();
        auto* uring = coop::GetUring();
        coop::io::Descriptor server(fds[0], uring);
        coop::io::Descriptor client(fds[1], uring);

        for (auto _ : state)
        {
            coop::io::SendAll(client, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1);

            {
                coop::http::PlaintextTransport transport(server);
                auto conn = ctx->Allocate<HttpConn>(HTTP_BUF,
                    transport, ctx, co, HTTP_BUF);
                conn->GetRequestLine();
                conn->Send(200, "application/json", RESP_4K, sizeof(RESP_4K) - 1);
            }

            DrainFd(fds[1]);
        }
    });
}
BENCHMARK(BM_Http_Tcp_Response4K);

// ---------------------------------------------------------------------------
// TLS HTTP benchmarks — full-stack: TLS encryption + HTTP parse + response
//
// These measure what users actually experience: Connection<TlsTransport>
// parsing encrypted HTTP traffic. Client runs on a separate context because
// TLS I/O is cooperative (both sides need to drive the handshake and
// encrypt/decrypt data).
//
// Uses kTLS (socket BIO) over TCP — the production path from RunTlsServer.
// ---------------------------------------------------------------------------

// TLS contexts need larger stacks for the SSL state.
//
static constexpr coop::SpawnConfiguration s_tlsConfig = {
    .priority = 0,
    .stackSize = 65536,
};

namespace
{

struct HttpCertData
{
    char cert[8192];
    int certLen;
    char key[8192];
    int keyLen;
    bool loaded;
};

HttpCertData s_httpCerts = {};

void EnsureHttpCertsLoaded()
{
    if (s_httpCerts.loaded) return;
    s_httpCerts.certLen = coop::io::ReadFile(
        "cert.pem", s_httpCerts.cert, sizeof(s_httpCerts.cert));
    assert(s_httpCerts.certLen > 0);
    s_httpCerts.keyLen = coop::io::ReadFile(
        "key.pem", s_httpCerts.key, sizeof(s_httpCerts.key));
    assert(s_httpCerts.keyLen > 0);
    s_httpCerts.loaded = true;
}

struct HttpSSLContextPair
{
    coop::io::ssl::Context server{coop::io::ssl::Mode::Server};
    coop::io::ssl::Context client{coop::io::ssl::Mode::Client};

    HttpSSLContextPair()
    {
        [[maybe_unused]] bool r;
        r = server.LoadCertificate(s_httpCerts.cert, s_httpCerts.certLen);
        assert(r);
        r = server.LoadPrivateKey(s_httpCerts.key, s_httpCerts.keyLen);
        assert(r);
        server.EnableKTLS();
        client.EnableKTLS();
    }
};

} // end anonymous namespace

// Helper: run a TLS HTTP benchmark. Sets up TCP pair, handshakes both sides on separate
// contexts, then calls the benchmark body with the established TLS connections.
//
// `clientFn` runs on a spawned context and sends encrypted HTTP requests.
// `serverFn` runs on the main context and parses/responds via Connection<TlsTransport>.
//
// Cert loading happens once per process via EnsureCertsLoaded.
//
static void RunTlsBenchmark(
    benchmark::State& state,
    std::function<void(coop::Context*, benchmark::State&,
                       coop::io::ssl::Connection&, coop::io::Descriptor&,
                       coop::io::ssl::Connection&, coop::io::Descriptor&)> fn)
{
    std::function<void(coop::Context*, benchmark::State&)> wrapper =
        [&fn](coop::Context* ctx, benchmark::State& state)
    {
        EnsureHttpCertsLoaded();
        HttpSSLContextPair ssl;

        int fds[2];
        MakeTcpPair(fds);

        coop::io::Descriptor serverDesc(fds[0]);
        coop::io::Descriptor clientDesc(fds[1]);

        coop::io::ssl::Connection serverConn(
            ssl.server, serverDesc, coop::io::ssl::SocketBio{});
        coop::io::ssl::Connection clientConn(
            ssl.client, clientDesc, coop::io::ssl::SocketBio{});

        // Handshake both sides cooperatively.
        //
        bool handshakeDone = false;
        ctx->GetCooperator()->Spawn(s_tlsConfig, [&](coop::Context*)
        {
            [[maybe_unused]] int r = serverConn.Handshake();
            assert(r == 0);
            handshakeDone = true;
        });

        [[maybe_unused]] int r = clientConn.Handshake();
        assert(r == 0);
        while (!handshakeDone) ctx->Yield(true);

        fn(ctx, state, serverConn, serverDesc, clientConn, clientDesc);
    };

    RunBenchmark(state, wrapper, s_tlsConfig);
}

// TLS client pump: sends requests and drains responses until `done` is set.
// Runs on a spawned context. The server-side benchmark loop drives iteration count.
// On exit from the benchmark loop, the server closes its side of the TCP pair,
// which causes the client's next SSL_read to fail, breaking the pump.
//
// Drains each response by scanning for the end of headers (\r\n\r\n), extracting
// Content-Length, then reading exactly that many body bytes.
//
static void TlsClientPump(
    coop::io::ssl::Connection& clientConn,
    const char* request, size_t requestLen,
    bool& done, bool& exited)
{
    char buf[8192];
    size_t buffered = 0;

    while (!done)
    {
        int r = coop::io::ssl::SendAll(clientConn, request, requestLen);
        if (r <= 0) break;

        // Recv until we have a complete response. Small responses fit in one TLS record;
        // larger ones may need multiple reads.
        //
        while (true)
        {
            int n = coop::io::ssl::Recv(clientConn, buf + buffered, sizeof(buf) - buffered);
            if (n <= 0) goto out;
            buffered += n;

            // Find \r\n\r\n — end of headers
            //
            auto* end = static_cast<char*>(
                memmem(buf, buffered, "\r\n\r\n", 4));
            if (!end) continue;

            size_t headerEnd = (end - buf) + 4;

            // Extract Content-Length
            //
            size_t contentLength = 0;
            auto* cl = static_cast<char*>(
                memmem(buf, headerEnd, "Content-Length: ", 16));
            if (cl) contentLength = strtoul(cl + 16, nullptr, 10);

            size_t totalResponse = headerEnd + contentLength;
            if (buffered >= totalResponse)
            {
                // Carry over leftover bytes from the next response
                //
                size_t leftover = buffered - totalResponse;
                if (leftover > 0) memmove(buf, buf + totalResponse, leftover);
                buffered = leftover;
                break;
            }
        }
    }
out:
    exited = true;
}

// TLS minimal GET — parse request line + send response. Direct comparison with
// BM_Http_Tcp_MinimalGet.
//
static void BM_Http_Tls_MinimalGet(benchmark::State& state)
{
    RunTlsBenchmark(state,
        [](coop::Context* ctx, benchmark::State& state,
           coop::io::ssl::Connection& serverConn, coop::io::Descriptor& serverDesc,
           coop::io::ssl::Connection& clientConn, coop::io::Descriptor& clientDesc)
    {
        auto* co = ctx->GetCooperator();
        bool done = false, exited = false;

        co->Spawn(s_tlsConfig, [&](coop::Context*)
        {
            TlsClientPump(clientConn, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1,
                done, exited);
        });

        for (auto _ : state)
        {
            coop::http::TlsTransport transport(serverConn, serverDesc);
            auto conn = ctx->Allocate<HttpTlsConn>(HTTP_BUF,
                transport, ctx, co, HTTP_BUF, std::chrono::seconds(0));
            auto* req = conn->GetRequestLine();
            assert(req);
            conn->Send(200, "text/plain", RESP_BODY, sizeof(RESP_BODY) - 1);
        }

        done = true;
        shutdown(serverDesc.m_fd, SHUT_RDWR);
        while (!exited) ctx->Yield(true);
    });
}
BENCHMARK(BM_Http_Tls_MinimalGet);

// TLS realistic GET — parse args + search headers + send JSON. Direct comparison with
// BM_Http_Tcp_RealisticGet.
//
static void BM_Http_Tls_RealisticGet(benchmark::State& state)
{
    InitResponseBodies();

    RunTlsBenchmark(state,
        [](coop::Context* ctx, benchmark::State& state,
           coop::io::ssl::Connection& serverConn, coop::io::Descriptor& serverDesc,
           coop::io::ssl::Connection& clientConn, coop::io::Descriptor& clientDesc)
    {
        auto* co = ctx->GetCooperator();
        bool done = false, exited = false;

        co->Spawn(s_tlsConfig, [&](coop::Context*)
        {
            TlsClientPump(clientConn,
                REQ_REALISTIC_GET, sizeof(REQ_REALISTIC_GET) - 1, done, exited);
        });

        for (auto _ : state)
        {
            coop::http::TlsTransport transport(serverConn, serverDesc);
            auto conn = ctx->Allocate<HttpTlsConn>(HTTP_BUF,
                transport, ctx, co, HTTP_BUF, std::chrono::seconds(0));
            auto* req = conn->GetRequestLine();
            assert(req);

            while (auto* name = conn->NextArgName())
            {
                auto* val = conn->ReadArgValue();
                benchmark::DoNotOptimize(val);
            }

            bool authorized = false;
            while (auto* name = conn->NextHeaderName())
            {
                if (strncasecmp(name, "Authorization", 13) == 0)
                {
                    auto* val = conn->ReadHeaderValue();
                    benchmark::DoNotOptimize(val);
                    authorized = true;
                }
                else
                {
                    conn->SkipHeaderValue();
                }
            }
            benchmark::DoNotOptimize(authorized);

            conn->Send(200, "application/json",
                RESP_JSON_SMALL, sizeof(RESP_JSON_SMALL) - 1);
        }

        done = true;
        shutdown(serverDesc.m_fd, SHUT_RDWR);
        while (!exited) ctx->Yield(true);
    });
}
BENCHMARK(BM_Http_Tls_RealisticGet);

// TLS realistic POST — parse headers + consume body + respond. Direct comparison with
// BM_Http_Tcp_RealisticPost.
//
static void BM_Http_Tls_RealisticPost(benchmark::State& state)
{
    RunTlsBenchmark(state,
        [](coop::Context* ctx, benchmark::State& state,
           coop::io::ssl::Connection& serverConn, coop::io::Descriptor& serverDesc,
           coop::io::ssl::Connection& clientConn, coop::io::Descriptor& clientDesc)
    {
        auto* co = ctx->GetCooperator();
        bool done = false, exited = false;

        co->Spawn(s_tlsConfig, [&](coop::Context*)
        {
            TlsClientPump(clientConn,
                REQ_REALISTIC_POST, sizeof(REQ_REALISTIC_POST) - 1, done, exited);
        });

        for (auto _ : state)
        {
            coop::http::TlsTransport transport(serverConn, serverDesc);
            auto conn = ctx->Allocate<HttpTlsConn>(HTTP_BUF,
                transport, ctx, co, HTTP_BUF, std::chrono::seconds(0));
            auto* req = conn->GetRequestLine();
            assert(req);

            while (auto* name = conn->NextHeaderName())
            {
                if (strncasecmp(name, "Content-Type", 12) == 0 ||
                    strncasecmp(name, "Authorization", 13) == 0)
                {
                    auto* val = conn->ReadHeaderValue();
                    benchmark::DoNotOptimize(val);
                }
                else
                {
                    conn->SkipHeaderValue();
                }
            }

            while (auto* chunk = conn->ReadBody())
            {
                benchmark::DoNotOptimize(chunk);
            }

            conn->Send(200, "application/json",
                RESP_JSON_SMALL, sizeof(RESP_JSON_SMALL) - 1);
        }

        done = true;
        shutdown(serverDesc.m_fd, SHUT_RDWR);
        while (!exited) ctx->Yield(true);
    });
}
BENCHMARK(BM_Http_Tls_RealisticPost);

// TLS response size scaling — 1KB and 4KB.
//
static void BM_Http_Tls_Response1K(benchmark::State& state)
{
    InitResponseBodies();

    RunTlsBenchmark(state,
        [](coop::Context* ctx, benchmark::State& state,
           coop::io::ssl::Connection& serverConn, coop::io::Descriptor& serverDesc,
           coop::io::ssl::Connection& clientConn, coop::io::Descriptor& clientDesc)
    {
        auto* co = ctx->GetCooperator();
        bool done = false, exited = false;

        co->Spawn(s_tlsConfig, [&](coop::Context*)
        {
            TlsClientPump(clientConn, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1,
                done, exited);
        });

        for (auto _ : state)
        {
            coop::http::TlsTransport transport(serverConn, serverDesc);
            auto conn = ctx->Allocate<HttpTlsConn>(HTTP_BUF,
                transport, ctx, co, HTTP_BUF, std::chrono::seconds(0));
            conn->GetRequestLine();
            conn->Send(200, "application/json", RESP_1K, sizeof(RESP_1K) - 1);
        }

        done = true;
        shutdown(serverDesc.m_fd, SHUT_RDWR);
        while (!exited) ctx->Yield(true);
    });
}
BENCHMARK(BM_Http_Tls_Response1K);

static void BM_Http_Tls_Response4K(benchmark::State& state)
{
    InitResponseBodies();

    RunTlsBenchmark(state,
        [](coop::Context* ctx, benchmark::State& state,
           coop::io::ssl::Connection& serverConn, coop::io::Descriptor& serverDesc,
           coop::io::ssl::Connection& clientConn, coop::io::Descriptor& clientDesc)
    {
        auto* co = ctx->GetCooperator();
        bool done = false, exited = false;

        co->Spawn(s_tlsConfig, [&](coop::Context*)
        {
            TlsClientPump(clientConn, REQ_MINIMAL, sizeof(REQ_MINIMAL) - 1,
                done, exited);
        });

        for (auto _ : state)
        {
            coop::http::TlsTransport transport(serverConn, serverDesc);
            auto conn = ctx->Allocate<HttpTlsConn>(HTTP_BUF,
                transport, ctx, co, HTTP_BUF, std::chrono::seconds(0));
            conn->GetRequestLine();
            conn->Send(200, "application/json", RESP_4K, sizeof(RESP_4K) - 1);
        }

        done = true;
        shutdown(serverDesc.m_fd, SHUT_RDWR);
        while (!exited) ctx->Yield(true);
    });
}
BENCHMARK(BM_Http_Tls_Response4K);
