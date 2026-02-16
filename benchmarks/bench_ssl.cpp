#include <cassert>
#include <functional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/thread.h"

#include "coop/io/descriptor.h"
#include "coop/io/io.h"
#include "coop/io/read_file.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/io/ssl/ssl.h"

// ---------------------------------------------------------------------------
// SSL/TLS benchmarks
//
// Measures TLS overhead on top of the cooperative IO path. Each benchmark
// has a plaintext counterpart in bench_io.cpp for direct comparison.
//
// Naming: BM_SSL_{Shape}
//   Shape: Handshake, RoundTrip, PingPong
// ---------------------------------------------------------------------------

static constexpr int MSG_SIZE = 64;

// TLS connections embed a 16KB staging buffer. Contexts running TLS need larger stacks.
//
static constexpr coop::SpawnConfiguration s_tlsSpawnConfig = {
    .priority = 0,
    .stackSize = 65536,
};

// ---------------------------------------------------------------------------
// Cert loading — done once per process, reused across all benchmarks
// ---------------------------------------------------------------------------

struct CertData
{
    char cert[8192];
    int certLen;
    char key[8192];
    int keyLen;
    bool loaded;
};

static CertData s_certs = {};

// Load certs from the build output directory. Must be called from inside a cooperator
// (io::ReadFile is cooperative). Safe to call multiple times — idempotent.
//
static void EnsureCertsLoaded()
{
    if (s_certs.loaded) return;
    s_certs.certLen = coop::io::ReadFile("cert.pem", s_certs.cert, sizeof(s_certs.cert));
    assert(s_certs.certLen > 0);
    s_certs.keyLen = coop::io::ReadFile("key.pem", s_certs.key, sizeof(s_certs.key));
    assert(s_certs.keyLen > 0);
    s_certs.loaded = true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct BenchmarkArgs
{
    benchmark::State* state;
    std::function<void(coop::Context*, benchmark::State&)>* fn;
};

static void RunBenchmark(benchmark::State& state,
    std::function<void(coop::Context*, benchmark::State&)> fn)
{
    coop::Cooperator cooperator;
    coop::Thread t(&cooperator);

    BenchmarkArgs args;
    args.state = &state;
    args.fn = &fn;

    cooperator.Submit([](coop::Context* ctx, void* arg)
    {
        auto* a = static_cast<BenchmarkArgs*>(arg);
        EnsureCertsLoaded();
        (*a->fn)(ctx, *a->state);
        ctx->GetCooperator()->Shutdown();
    }, &args, s_tlsSpawnConfig);
}

static void MakeSocketPair(int fds[2])
{
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(ret == 0);
    (void)ret;
}

// Create a TCP socket pair over loopback. Required for kTLS (kernel TLS only works on TCP, not
// AF_UNIX). Uses synchronous POSIX calls — both connect() and accept() complete instantly on
// loopback.
//
static void MakeTcpPair(int fds[2])
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // kernel picks ephemeral port
    //
    [[maybe_unused]] int r;
    r = bind(listener, (struct sockaddr*)&addr, sizeof(addr));
    assert(r == 0);
    r = listen(listener, 1);
    assert(r == 0);

    // Retrieve the assigned port
    //
    socklen_t addrLen = sizeof(addr);
    r = getsockname(listener, (struct sockaddr*)&addr, &addrLen);
    assert(r == 0);

    // Connect synchronously — instant on loopback
    //
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(client >= 0);
    r = connect(client, (struct sockaddr*)&addr, sizeof(addr));
    assert(r == 0);

    // Accept — connection already established
    //
    int server = accept(listener, nullptr, nullptr);
    assert(server >= 0);

    close(listener);
    fds[0] = server;
    fds[1] = client;
}

// Create server and client SSL contexts with certs loaded.
//
struct SSLContextPair
{
    coop::io::ssl::Context server{coop::io::ssl::Mode::Server};
    coop::io::ssl::Context client{coop::io::ssl::Mode::Client};

    SSLContextPair()
    {
        [[maybe_unused]] bool r;
        r = server.LoadCertificate(s_certs.cert, s_certs.certLen);
        assert(r);
        r = server.LoadPrivateKey(s_certs.key, s_certs.keyLen);
        assert(r);
    }
};

// ---------------------------------------------------------------------------
// Shape: Handshake
//
// Measures the cost of a full TLS handshake (connection setup). Each iteration
// creates a new socketpair, wraps both ends in TLS, and handshakes. This is
// the expensive per-connection cost that amortizes over the session lifetime.
// ---------------------------------------------------------------------------

static void BM_SSL_Handshake(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        SSLContextPair ssl;

        for (auto _ : state)
        {
            int fds[2];
            MakeSocketPair(fds);

            coop::io::Descriptor serverDesc(fds[0]);
            coop::io::Descriptor clientDesc(fds[1]);

            char serverBuf[coop::io::ssl::Connection::BUFFER_SIZE];
            char clientBuf[coop::io::ssl::Connection::BUFFER_SIZE];

            coop::io::ssl::Connection serverConn(
                ssl.server, serverDesc, serverBuf, sizeof(serverBuf));
            coop::io::ssl::Connection clientConn(
                ssl.client, clientDesc, clientBuf, sizeof(clientBuf));

            // Both ends handshake cooperatively. The server blocks on WANT_READ,
            // yields, the client drives its half, and they alternate until done.
            //
            bool serverDone = false;
            ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
            {
                [[maybe_unused]] int r = serverConn.Handshake();
                assert(r == 0);
                serverDone = true;
            });

            [[maybe_unused]] int r = clientConn.Handshake();
            assert(r == 0);
            while (!serverDone) ctx->Yield(true);
        }
    });
}
BENCHMARK(BM_SSL_Handshake);

// ---------------------------------------------------------------------------
// Shape: PingPong (the natural TLS shape)
//
// Two contexts, one TLS connection. Echo responder: Recv -> Send. Driver:
// Send -> Recv. Each iteration = 1 encrypted round-trip through two contexts.
// Compare with BM_IO_PingPong for plaintext baseline.
//
// This is the primary TLS benchmark — TLS inherently requires two contexts
// because the BIO bridge needs both ends to cooperate.
// ---------------------------------------------------------------------------

static void BM_SSL_PingPong(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        SSLContextPair ssl;

        int fds[2];
        MakeSocketPair(fds);

        coop::io::Descriptor serverDesc(fds[0]);
        coop::io::Descriptor clientDesc(fds[1]);

        char serverBuf[coop::io::ssl::Connection::BUFFER_SIZE];
        char clientBuf[coop::io::ssl::Connection::BUFFER_SIZE];

        coop::io::ssl::Connection serverConn(
            ssl.server, serverDesc, serverBuf, sizeof(serverBuf));
        coop::io::ssl::Connection clientConn(
            ssl.client, clientDesc, clientBuf, sizeof(clientBuf));

        bool done = false;
        bool responderExited = false;

        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            [[maybe_unused]] int r = serverConn.Handshake();
            assert(r == 0);

            char buf[MSG_SIZE] = {};
            while (!done)
            {
                int n = coop::io::ssl::Recv(serverConn, buf, MSG_SIZE);
                if (done || n <= 0) break;
                coop::io::ssl::Send(serverConn, buf, n);
            }

            responderExited = true;
        });

        [[maybe_unused]] int r = clientConn.Handshake();
        assert(r == 0);

        char msg[MSG_SIZE] = {};
        char buf[MSG_SIZE] = {};

        for (auto _ : state)
        {
            coop::io::ssl::Send(clientConn, msg, MSG_SIZE);
            coop::io::ssl::Recv(clientConn, buf, MSG_SIZE);
        }

        // Drain the responder
        //
        done = true;
        coop::io::ssl::Send(clientConn, msg, MSG_SIZE);
        while (!responderExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_SSL_PingPong);

// ---------------------------------------------------------------------------
// Shape: PingPong with varying message sizes
//
// Same as PingPong but parameterized by message size. Captures how TLS record
// framing and encryption overhead scales with payload.
// ---------------------------------------------------------------------------

static void BM_SSL_PingPong_MsgSize(benchmark::State& state)
{
    const int msgSize = state.range(0);

    RunBenchmark(state, [msgSize](coop::Context* ctx, benchmark::State& state)
    {
        SSLContextPair ssl;

        int fds[2];
        MakeSocketPair(fds);

        coop::io::Descriptor serverDesc(fds[0]);
        coop::io::Descriptor clientDesc(fds[1]);

        char serverBuf[coop::io::ssl::Connection::BUFFER_SIZE];
        char clientBuf[coop::io::ssl::Connection::BUFFER_SIZE];

        coop::io::ssl::Connection serverConn(
            ssl.server, serverDesc, serverBuf, sizeof(serverBuf));
        coop::io::ssl::Connection clientConn(
            ssl.client, clientDesc, clientBuf, sizeof(clientBuf));

        bool done = false;
        bool responderExited = false;

        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            [[maybe_unused]] int r = serverConn.Handshake();
            assert(r == 0);

            char buf[16384] = {};
            while (!done)
            {
                int n = coop::io::ssl::Recv(serverConn, buf, sizeof(buf));
                if (done || n <= 0) break;
                coop::io::ssl::SendAll(serverConn, buf, n);
            }

            responderExited = true;
        });

        [[maybe_unused]] int r = clientConn.Handshake();
        assert(r == 0);

        // Heap-allocate for larger message sizes
        //
        std::vector<char> msg(msgSize, 'A');
        std::vector<char> buf(msgSize);

        for (auto _ : state)
        {
            coop::io::ssl::SendAll(clientConn, msg.data(), msgSize);

            // Recv may return less than msgSize per call; drain fully
            //
            int remaining = msgSize;
            while (remaining > 0)
            {
                int n = coop::io::ssl::Recv(clientConn, buf.data(), remaining);
                assert(n > 0);
                remaining -= n;
            }
        }

        state.SetBytesProcessed(
            static_cast<int64_t>(state.iterations()) * msgSize * 2);

        done = true;
        coop::io::ssl::Send(clientConn, msg.data(), 1);
        while (!responderExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_SSL_PingPong_MsgSize)
    ->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(16384);

// ---------------------------------------------------------------------------
// Shape: Plaintext PingPong with varying message sizes (baseline)
//
// Direct comparison for BM_SSL_PingPong_MsgSize — same structure, no TLS.
// ---------------------------------------------------------------------------

static void BM_Plaintext_PingPong_MsgSize(benchmark::State& state)
{
    const int msgSize = state.range(0);

    RunBenchmark(state, [msgSize](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeSocketPair(fds);

        coop::io::Descriptor a(fds[0]);
        coop::io::Descriptor b(fds[1]);

        bool done = false;
        bool responderExited = false;

        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            char buf[16384] = {};
            while (!done)
            {
                int n = coop::io::Recv(b, buf, sizeof(buf));
                if (done || n <= 0) break;

                int at = 0;
                while (at < n)
                {
                    int sent = coop::io::Send(b, buf + at, n - at);
                    if (sent <= 0) return;
                    at += sent;
                }
            }

            responderExited = true;
        });

        std::vector<char> msg(msgSize, 'A');
        std::vector<char> buf(msgSize);

        for (auto _ : state)
        {
            // SendAll equivalent
            //
            int at = 0;
            while (at < msgSize)
            {
                int sent = coop::io::Send(a, msg.data() + at, msgSize - at);
                assert(sent > 0);
                at += sent;
            }

            int remaining = msgSize;
            while (remaining > 0)
            {
                int n = coop::io::Recv(a, buf.data(), remaining);
                assert(n > 0);
                remaining -= n;
            }
        }

        state.SetBytesProcessed(
            static_cast<int64_t>(state.iterations()) * msgSize * 2);

        done = true;
        coop::io::Send(a, msg.data(), 1);
        while (!responderExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_Plaintext_PingPong_MsgSize)
    ->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(16384);

// ---------------------------------------------------------------------------
// kTLS benchmarks — TCP socket pair with kernel TLS offload
//
// These require TCP (not AF_UNIX) because kTLS is a kernel feature that only
// works on TCP sockets. Compare with BM_SSL_PingPong (memory BIO over AF_UNIX)
// and BM_Plaintext_PingPong_MsgSize (no TLS) for the full picture.
// ---------------------------------------------------------------------------

// kTLS PingPong — same as BM_SSL_PingPong but over TCP with kTLS enabled.
// If kTLS activates, Send/Recv bypass OpenSSL entirely (kernel handles crypto).
//
static void BM_SSL_PingPong_kTLS(benchmark::State& state)
{
    RunBenchmark(state, [](coop::Context* ctx, benchmark::State& state)
    {
        SSLContextPair ssl;
        ssl.server.EnableKTLS();
        ssl.client.EnableKTLS();

        int fds[2];
        MakeTcpPair(fds);

        coop::io::Descriptor serverDesc(fds[0]);
        coop::io::Descriptor clientDesc(fds[1]);

        coop::io::ssl::Connection serverConn(
            ssl.server, serverDesc, coop::io::ssl::SocketBio{});
        coop::io::ssl::Connection clientConn(
            ssl.client, clientDesc, coop::io::ssl::SocketBio{});

        bool done = false;
        bool responderExited = false;

        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            [[maybe_unused]] int r = serverConn.Handshake();
            assert(r == 0);

            char buf[MSG_SIZE] = {};
            while (!done)
            {
                int n = coop::io::ssl::Recv(serverConn, buf, MSG_SIZE);
                if (done || n <= 0) break;
                coop::io::ssl::Send(serverConn, buf, n);
            }

            responderExited = true;
        });

        [[maybe_unused]] int r = clientConn.Handshake();
        assert(r == 0);

        char msg[MSG_SIZE] = {};
        char buf[MSG_SIZE] = {};

        for (auto _ : state)
        {
            coop::io::ssl::Send(clientConn, msg, MSG_SIZE);
            coop::io::ssl::Recv(clientConn, buf, MSG_SIZE);
        }

        done = true;
        coop::io::ssl::Send(clientConn, msg, MSG_SIZE);
        while (!responderExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_SSL_PingPong_kTLS);

// kTLS PingPong with varying message sizes — primary throughput benchmark.
//
static void BM_SSL_PingPong_MsgSize_kTLS(benchmark::State& state)
{
    const int msgSize = state.range(0);

    RunBenchmark(state, [msgSize](coop::Context* ctx, benchmark::State& state)
    {
        SSLContextPair ssl;
        ssl.server.EnableKTLS();
        ssl.client.EnableKTLS();

        int fds[2];
        MakeTcpPair(fds);

        coop::io::Descriptor serverDesc(fds[0]);
        coop::io::Descriptor clientDesc(fds[1]);

        coop::io::ssl::Connection serverConn(
            ssl.server, serverDesc, coop::io::ssl::SocketBio{});
        coop::io::ssl::Connection clientConn(
            ssl.client, clientDesc, coop::io::ssl::SocketBio{});

        bool done = false;
        bool responderExited = false;

        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            [[maybe_unused]] int r = serverConn.Handshake();
            assert(r == 0);

            char buf[16384] = {};
            while (!done)
            {
                int n = coop::io::ssl::Recv(serverConn, buf, sizeof(buf));
                if (done || n <= 0) break;
                coop::io::ssl::SendAll(serverConn, buf, n);
            }

            responderExited = true;
        });

        [[maybe_unused]] int r = clientConn.Handshake();
        assert(r == 0);

        std::vector<char> msg(msgSize, 'A');
        std::vector<char> buf(msgSize);

        for (auto _ : state)
        {
            coop::io::ssl::SendAll(clientConn, msg.data(), msgSize);

            int remaining = msgSize;
            while (remaining > 0)
            {
                int n = coop::io::ssl::Recv(clientConn, buf.data(), remaining);
                assert(n > 0);
                remaining -= n;
            }
        }

        state.SetBytesProcessed(
            static_cast<int64_t>(state.iterations()) * msgSize * 2);

        done = true;
        coop::io::ssl::Send(clientConn, msg.data(), 1);
        while (!responderExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_SSL_PingPong_MsgSize_kTLS)
    ->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(16384);

// Socket BIO PingPong without kTLS — isolates BIO overhead vs memory BIO path.
// Uses TCP but does NOT call EnableKTLS(), so SSL_write/SSL_read + io::Poll.
//
static void BM_SSL_PingPong_SocketBio(benchmark::State& state)
{
    const int msgSize = state.range(0);

    RunBenchmark(state, [msgSize](coop::Context* ctx, benchmark::State& state)
    {
        SSLContextPair ssl;

        int fds[2];
        MakeTcpPair(fds);

        coop::io::Descriptor serverDesc(fds[0]);
        coop::io::Descriptor clientDesc(fds[1]);

        // Socket BIO but no kTLS — pure SSL_write/SSL_read + io::Poll path
        //
        coop::io::ssl::Connection serverConn(
            ssl.server, serverDesc, coop::io::ssl::SocketBio{});
        coop::io::ssl::Connection clientConn(
            ssl.client, clientDesc, coop::io::ssl::SocketBio{});

        bool done = false;
        bool responderExited = false;

        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            [[maybe_unused]] int r = serverConn.Handshake();
            assert(r == 0);

            char buf[16384] = {};
            while (!done)
            {
                int n = coop::io::ssl::Recv(serverConn, buf, sizeof(buf));
                if (done || n <= 0) break;
                coop::io::ssl::SendAll(serverConn, buf, n);
            }

            responderExited = true;
        });

        [[maybe_unused]] int r = clientConn.Handshake();
        assert(r == 0);

        std::vector<char> msg(msgSize, 'A');
        std::vector<char> buf(msgSize);

        for (auto _ : state)
        {
            coop::io::ssl::SendAll(clientConn, msg.data(), msgSize);

            int remaining = msgSize;
            while (remaining > 0)
            {
                int n = coop::io::ssl::Recv(clientConn, buf.data(), remaining);
                assert(n > 0);
                remaining -= n;
            }
        }

        state.SetBytesProcessed(
            static_cast<int64_t>(state.iterations()) * msgSize * 2);

        done = true;
        coop::io::ssl::Send(clientConn, msg.data(), 1);
        while (!responderExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_SSL_PingPong_SocketBio)
    ->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(16384);

// ---------------------------------------------------------------------------
// Streaming throughput benchmarks
//
// Measure sustained throughput (MB/s), not round-trip latency. One context
// sends continuously, another drains. This reveals per-byte costs (crypto,
// copies) that are invisible in ping-pong where per-operation overhead
// dominates.
// ---------------------------------------------------------------------------

static constexpr int64_t STREAM_BYTES = 4 * 1024 * 1024; // 4MB per iteration

// Memory BIO streaming throughput (AF_UNIX)
//
static void BM_SSL_Throughput_MemBio(benchmark::State& state)
{
    const int msgSize = state.range(0);

    RunBenchmark(state, [msgSize](coop::Context* ctx, benchmark::State& state)
    {
        SSLContextPair ssl;

        int fds[2];
        MakeSocketPair(fds);

        coop::io::Descriptor serverDesc(fds[0]);
        coop::io::Descriptor clientDesc(fds[1]);

        char serverBuf[coop::io::ssl::Connection::BUFFER_SIZE];
        char clientBuf[coop::io::ssl::Connection::BUFFER_SIZE];

        coop::io::ssl::Connection serverConn(
            ssl.server, serverDesc, serverBuf, sizeof(serverBuf));
        coop::io::ssl::Connection clientConn(
            ssl.client, clientDesc, clientBuf, sizeof(clientBuf));

        bool done = false;
        bool drainerExited = false;

        // Drainer: reads and discards
        //
        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            [[maybe_unused]] int r = serverConn.Handshake();
            assert(r == 0);

            char buf[16384];
            while (!done)
            {
                int n = coop::io::ssl::Recv(serverConn, buf, sizeof(buf));
                if (n <= 0) break;
            }

            drainerExited = true;
        });

        [[maybe_unused]] int r = clientConn.Handshake();
        assert(r == 0);

        std::vector<char> msg(msgSize, 'A');

        for (auto _ : state)
        {
            int64_t sent = 0;
            while (sent < STREAM_BYTES)
            {
                int n = coop::io::ssl::SendAll(
                    clientConn, msg.data(), msgSize);
                assert(n == msgSize);
                sent += n;
            }
        }

        state.SetBytesProcessed(
            static_cast<int64_t>(state.iterations()) * STREAM_BYTES);

        done = true;
        // Close client side to unblock drainer
        //
        SSL_shutdown(clientConn.m_ssl);
        while (!drainerExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_SSL_Throughput_MemBio)
    ->Arg(1024)->Arg(4096)->Arg(16384);

// kTLS streaming throughput (TCP)
//
static void BM_SSL_Throughput_kTLS(benchmark::State& state)
{
    const int msgSize = state.range(0);

    RunBenchmark(state, [msgSize](coop::Context* ctx, benchmark::State& state)
    {
        SSLContextPair ssl;
        ssl.server.EnableKTLS();
        ssl.client.EnableKTLS();

        int fds[2];
        MakeTcpPair(fds);

        coop::io::Descriptor serverDesc(fds[0]);
        coop::io::Descriptor clientDesc(fds[1]);

        coop::io::ssl::Connection serverConn(
            ssl.server, serverDesc, coop::io::ssl::SocketBio{});
        coop::io::ssl::Connection clientConn(
            ssl.client, clientDesc, coop::io::ssl::SocketBio{});

        bool done = false;
        bool drainerExited = false;

        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            [[maybe_unused]] int r = serverConn.Handshake();
            assert(r == 0);

            char buf[16384];
            while (!done)
            {
                int n = coop::io::ssl::Recv(serverConn, buf, sizeof(buf));
                if (n <= 0) break;
            }

            drainerExited = true;
        });

        [[maybe_unused]] int r = clientConn.Handshake();
        assert(r == 0);

        std::vector<char> msg(msgSize, 'A');

        for (auto _ : state)
        {
            int64_t sent = 0;
            while (sent < STREAM_BYTES)
            {
                int n = coop::io::ssl::SendAll(
                    clientConn, msg.data(), msgSize);
                assert(n == msgSize);
                sent += n;
            }
        }

        state.SetBytesProcessed(
            static_cast<int64_t>(state.iterations()) * STREAM_BYTES);

        done = true;
        SSL_shutdown(clientConn.m_ssl);
        while (!drainerExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_SSL_Throughput_kTLS)
    ->Arg(1024)->Arg(4096)->Arg(16384);

// Socket BIO streaming throughput (TCP, no kTLS)
//
static void BM_SSL_Throughput_SocketBio(benchmark::State& state)
{
    const int msgSize = state.range(0);

    RunBenchmark(state, [msgSize](coop::Context* ctx, benchmark::State& state)
    {
        SSLContextPair ssl;

        int fds[2];
        MakeTcpPair(fds);

        coop::io::Descriptor serverDesc(fds[0]);
        coop::io::Descriptor clientDesc(fds[1]);

        coop::io::ssl::Connection serverConn(
            ssl.server, serverDesc, coop::io::ssl::SocketBio{});
        coop::io::ssl::Connection clientConn(
            ssl.client, clientDesc, coop::io::ssl::SocketBio{});

        bool done = false;
        bool drainerExited = false;

        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            [[maybe_unused]] int r = serverConn.Handshake();
            assert(r == 0);

            char buf[16384];
            while (!done)
            {
                int n = coop::io::ssl::Recv(serverConn, buf, sizeof(buf));
                if (n <= 0) break;
            }

            drainerExited = true;
        });

        [[maybe_unused]] int r = clientConn.Handshake();
        assert(r == 0);

        std::vector<char> msg(msgSize, 'A');

        for (auto _ : state)
        {
            int64_t sent = 0;
            while (sent < STREAM_BYTES)
            {
                int n = coop::io::ssl::SendAll(
                    clientConn, msg.data(), msgSize);
                assert(n == msgSize);
                sent += n;
            }
        }

        state.SetBytesProcessed(
            static_cast<int64_t>(state.iterations()) * STREAM_BYTES);

        done = true;
        SSL_shutdown(clientConn.m_ssl);
        while (!drainerExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_SSL_Throughput_SocketBio)
    ->Arg(1024)->Arg(4096)->Arg(16384);

// Plaintext streaming throughput (AF_UNIX, baseline)
//
static void BM_Plaintext_Throughput(benchmark::State& state)
{
    const int msgSize = state.range(0);

    RunBenchmark(state, [msgSize](coop::Context* ctx, benchmark::State& state)
    {
        int fds[2];
        MakeSocketPair(fds);

        coop::io::Descriptor a(fds[0]);
        coop::io::Descriptor b(fds[1]);

        bool done = false;
        bool drainerExited = false;

        ctx->GetCooperator()->Spawn(s_tlsSpawnConfig, [&](coop::Context*)
        {
            char buf[16384];
            while (!done)
            {
                int n = coop::io::Recv(b, buf, sizeof(buf));
                if (n <= 0) break;
            }
            drainerExited = true;
        });

        std::vector<char> msg(msgSize, 'A');

        for (auto _ : state)
        {
            int64_t sent = 0;
            while (sent < STREAM_BYTES)
            {
                int n = coop::io::Send(a, msg.data(), msgSize);
                assert(n > 0);
                sent += n;
            }
        }

        state.SetBytesProcessed(
            static_cast<int64_t>(state.iterations()) * STREAM_BYTES);

        done = true;
        // Close sender to unblock drainer
        //
        coop::io::Close(a);
        while (!drainerExited) ctx->Yield(true);
    });
}
BENCHMARK(BM_Plaintext_Throughput)
    ->Arg(1024)->Arg(4096)->Arg(16384);
