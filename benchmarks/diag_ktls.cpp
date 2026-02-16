// Diagnostic: kTLS performance investigation
//
// Tests:
// 1. Plaintext baseline (no TLS) — establishes floor
// 2. kTLS TLS 1.3 (TX-only) — SSL_read RX, write() TX
// 3. kTLS TLS 1.2 (hopefully TX+RX) — read()/write() both directions
// 4. kTLS TLS 1.2 TX-only fallback — if RX doesn't activate

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <openssl/ssl.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/thread.h"

#include "coop/io/descriptor.h"
#include "coop/io/io.h"
#include "coop/io/read_file.h"
#include "coop/io/ssl/ssl.h"

using namespace coop;
using Clock = std::chrono::steady_clock;

static void MakeTcpPair(int fds[2], bool nodelay = false)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    [[maybe_unused]] int r;
    r = bind(listener, (struct sockaddr*)&addr, sizeof(addr));
    assert(r == 0);
    r = listen(listener, 1);
    assert(r == 0);
    socklen_t addrLen = sizeof(addr);
    r = getsockname(listener, (struct sockaddr*)&addr, &addrLen);
    assert(r == 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(client >= 0);
    r = connect(client, (struct sockaddr*)&addr, sizeof(addr));
    assert(r == 0);
    int server = accept(listener, nullptr, nullptr);
    assert(server >= 0);
    close(listener);

    if (nodelay)
    {
        int on = 1;
        setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }

    fds[0] = server;
    fds[1] = client;
}

// ---- POSIX helpers (blocking, no uring) ----

static int RecvPosixPoll(io::ssl::Connection& conn, void* buf, size_t size)
{
    for (;;)
    {
        int ret = SSL_read(conn.m_ssl, buf, size);
        if (ret > 0) return ret;
        int err = SSL_get_error(conn.m_ssl, ret);
        if (err == SSL_ERROR_WANT_READ)
        {
            struct pollfd pfd = {conn.m_desc.m_fd, POLLIN, 0};
            ::poll(&pfd, 1, -1);
        }
        else if (err == SSL_ERROR_WANT_WRITE)
        {
            struct pollfd pfd = {conn.m_desc.m_fd, POLLOUT, 0};
            ::poll(&pfd, 1, -1);
        }
        else return err == SSL_ERROR_ZERO_RETURN ? 0 : -1;
    }
}

// Direct write() on kTLS socket — kernel encrypts
//
static int SendKtlsDirect(int fd, const void* buf, size_t size)
{
    size_t at = 0;
    while (at < size)
    {
        ssize_t n = ::write(fd, (const char*)buf + at, size - at);
        if (n > 0) { at += n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct pollfd pfd = {fd, POLLOUT, 0};
            ::poll(&pfd, 1, -1);
            continue;
        }
        return -1;
    }
    return (int)size;
}

// Direct read() on kTLS socket — kernel decrypts
//
static int RecvKtlsDirect(int fd, void* buf, size_t size)
{
    for (;;)
    {
        ssize_t n = ::read(fd, buf, size);
        if (n > 0) return (int)n;
        if (n == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            struct pollfd pfd = {fd, POLLIN, 0};
            ::poll(&pfd, 1, -1);
            continue;
        }
        return -1;
    }
}

// Plaintext send/recv — no TLS at all
//
static int SendPlaintext(int fd, const void* buf, size_t size)
{
    return SendKtlsDirect(fd, buf, size); // same syscall, just no crypto
}

static int RecvPlaintext(int fd, void* buf, size_t size)
{
    return RecvKtlsDirect(fd, buf, size);
}

// ---- Ping-pong harness ----

struct PingPongConfig
{
    const char* name;
    // Function pointers for server and client send/recv
    int (*serverSend)(int fd, const void* buf, size_t size);
    int (*serverRecv)(int fd, void* buf, size_t size);
    int (*clientSend)(int fd, const void* buf, size_t size);
    int (*clientRecv)(int fd, void* buf, size_t size);
};

static void RunPingPong(const PingPongConfig& cfg, int serverFd, int clientFd)
{
    printf("  %8s  %10s\n", "Size", "Time(us)");

    for (int msgSize : {64, 256, 1024, 4096, 16384})
    {
        std::vector<char> msg(msgSize, 'A');
        std::vector<char> buf(msgSize);

        bool done = false;
        std::thread serverThread([&]()
        {
            char rbuf[16384];
            while (!done)
            {
                int n = cfg.serverRecv(serverFd, rbuf, sizeof(rbuf));
                if (n <= 0) break;
                cfg.serverSend(serverFd, rbuf, n);
            }
        });

        // Warm up
        cfg.clientSend(clientFd, msg.data(), msgSize);
        int remaining = msgSize;
        while (remaining > 0)
        {
            int n = cfg.clientRecv(clientFd, buf.data(), remaining);
            assert(n > 0);
            remaining -= n;
        }

        constexpr int ITERS = 500;
        auto t0 = Clock::now();
        for (int i = 0; i < ITERS; i++)
        {
            cfg.clientSend(clientFd, msg.data(), msgSize);
            remaining = msgSize;
            while (remaining > 0)
            {
                int n = cfg.clientRecv(clientFd, buf.data(), remaining);
                assert(n > 0);
                remaining -= n;
            }
        }
        auto t1 = Clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / ITERS;

        printf("  %8d  %10.1f\n", msgSize, us);

        done = true;
        cfg.clientSend(clientFd, msg.data(), 1); // unblock server
        serverThread.join();
    }
}

// SSL_read-based recv adapter (captures Connection*)
//
struct SslRecvAdapter
{
    io::ssl::Connection* conn;
    static int Recv(int /*fd*/, void* buf, size_t size)
    {
        // This is a hack — we use thread_local to pass the Connection*
        return RecvPosixPoll(*s_conn, buf, size);
    }
    static thread_local io::ssl::Connection* s_conn;
};
thread_local io::ssl::Connection* SslRecvAdapter::s_conn = nullptr;

int main()
{
    Cooperator cooperator;
    Thread t(&cooperator);

    cooperator.Submit([](Context* ctx, void*)
    {
        char certBuf[8192], keyBuf[8192];
        int certLen = io::ReadFile("cert.pem", certBuf, sizeof(certBuf));
        int keyLen = io::ReadFile("key.pem", keyBuf, sizeof(keyBuf));
        assert(certLen > 0 && keyLen > 0);

        printf("\n");

        // =============================================
        // Test 1: Plaintext baseline (no TLS at all)
        // =============================================
        printf("=== Plaintext baseline (no TLS) ===\n");
        {
            int fds[2];
            MakeTcpPair(fds);
            PingPongConfig cfg = {
                "plaintext", SendPlaintext, RecvPlaintext, SendPlaintext, RecvPlaintext
            };
            RunPingPong(cfg, fds[0], fds[1]);
            close(fds[0]);
            close(fds[1]);
        }

        // =============================================
        // Test 2: kTLS TLS 1.3 (TX-only, SSL_read RX)
        // =============================================
        printf("\n=== kTLS TLS 1.3 (TX-only: write() TX, SSL_read RX) ===\n");
        {
            io::ssl::Context serverCtx(io::ssl::Mode::Server);
            io::ssl::Context clientCtx(io::ssl::Mode::Client);
            serverCtx.LoadCertificate(certBuf, certLen);
            serverCtx.LoadPrivateKey(keyBuf, keyLen);
            serverCtx.EnableKTLS();
            clientCtx.EnableKTLS();

            int fds[2];
            MakeTcpPair(fds);
            io::Descriptor serverDesc(fds[0]);
            io::Descriptor clientDesc(fds[1]);
            io::ssl::Connection serverConn(serverCtx, serverDesc, io::ssl::SocketBio{});
            io::ssl::Connection clientConn(clientCtx, clientDesc, io::ssl::SocketBio{});

            // Handshake cooperatively
            bool serverReady = false;
            ctx->GetCooperator()->Spawn({.priority = 0, .stackSize = 65536}, [&](Context*)
            {
                [[maybe_unused]] int r = serverConn.Handshake();
                assert(r == 0);
                serverReady = true;
            });
            [[maybe_unused]] int r = clientConn.Handshake();
            assert(r == 0);
            while (!serverReady) ctx->Yield(true);

            printf("  Server ktls_tx=%d rx=%d, Client ktls_tx=%d rx=%d\n",
                serverConn.m_ktlsTx, serverConn.m_ktlsRx,
                clientConn.m_ktlsTx, clientConn.m_ktlsRx);
            printf("  TLS version: %s\n", SSL_get_version(clientConn.m_ssl));

            // Server: write() TX (kTLS), SSL_read RX
            // Client: write() TX (kTLS), SSL_read RX
            // Use thread_local hack for SSL_read adapter
            auto sslRecvServer = [&](int, void* buf, size_t size) -> int {
                return RecvPosixPoll(serverConn, buf, size);
            };
            auto sslRecvClient = [&](int, void* buf, size_t size) -> int {
                return RecvPosixPoll(clientConn, buf, size);
            };

            // Can't use lambdas as function pointers with captures, so use a simpler approach
            // Just run the ping-pong inline
            printf("  %8s  %10s\n", "Size", "Time(us)");
            for (int msgSize : {64, 256, 1024, 4096, 16384})
            {
                std::vector<char> msg(msgSize, 'A');
                std::vector<char> buf(msgSize);

                bool done = false;
                std::thread serverThread([&]()
                {
                    char rbuf[16384];
                    while (!done)
                    {
                        int n = RecvPosixPoll(serverConn, rbuf, sizeof(rbuf));
                        if (n <= 0) break;
                        SendKtlsDirect(serverConn.m_desc.m_fd, rbuf, n);
                    }
                });

                // Warm up
                SendKtlsDirect(clientConn.m_desc.m_fd, msg.data(), msgSize);
                int remaining = msgSize;
                while (remaining > 0)
                {
                    int n = RecvPosixPoll(clientConn, buf.data(), remaining);
                    assert(n > 0);
                    remaining -= n;
                }

                constexpr int ITERS = 500;
                auto t0 = Clock::now();
                for (int i = 0; i < ITERS; i++)
                {
                    SendKtlsDirect(clientConn.m_desc.m_fd, msg.data(), msgSize);
                    remaining = msgSize;
                    while (remaining > 0)
                    {
                        int n = RecvPosixPoll(clientConn, buf.data(), remaining);
                        assert(n > 0);
                        remaining -= n;
                    }
                }
                auto t1 = Clock::now();
                double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / ITERS;
                printf("  %8d  %10.1f\n", msgSize, us);

                done = true;
                SendKtlsDirect(clientConn.m_desc.m_fd, msg.data(), 1);
                serverThread.join();
            }
        }

        // =============================================
        // Test 3: kTLS TLS 1.2 (attempt full TX+RX)
        // =============================================
        printf("\n=== kTLS TLS 1.2 (attempt full TX+RX: read()/write() both) ===\n");
        {
            io::ssl::Context serverCtx(io::ssl::Mode::Server);
            io::ssl::Context clientCtx(io::ssl::Mode::Client);
            serverCtx.LoadCertificate(certBuf, certLen);
            serverCtx.LoadPrivateKey(keyBuf, keyLen);
            serverCtx.EnableKTLS();
            clientCtx.EnableKTLS();

            // Force TLS 1.2
            SSL_CTX_set_max_proto_version(serverCtx.m_ctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(clientCtx.m_ctx, TLS1_2_VERSION);

            int fds[2];
            MakeTcpPair(fds);
            io::Descriptor serverDesc(fds[0]);
            io::Descriptor clientDesc(fds[1]);
            io::ssl::Connection serverConn(serverCtx, serverDesc, io::ssl::SocketBio{});
            io::ssl::Connection clientConn(clientCtx, clientDesc, io::ssl::SocketBio{});

            bool serverReady = false;
            ctx->GetCooperator()->Spawn({.priority = 0, .stackSize = 65536}, [&](Context*)
            {
                [[maybe_unused]] int r = serverConn.Handshake();
                assert(r == 0);
                serverReady = true;
            });
            [[maybe_unused]] int r = clientConn.Handshake();
            assert(r == 0);
            while (!serverReady) ctx->Yield(true);

            printf("  Server ktls_tx=%d rx=%d, Client ktls_tx=%d rx=%d\n",
                serverConn.m_ktlsTx, serverConn.m_ktlsRx,
                clientConn.m_ktlsTx, clientConn.m_ktlsRx);
            printf("  TLS version: %s\n", SSL_get_version(clientConn.m_ssl));

            bool fullKtls = serverConn.m_ktlsTx && serverConn.m_ktlsRx
                         && clientConn.m_ktlsTx && clientConn.m_ktlsRx;

            if (fullKtls)
            {
                printf("  Full kTLS active! Using read()/write() both directions\n");
                PingPongConfig cfg = {
                    "ktls-full",
                    SendKtlsDirect, RecvKtlsDirect,
                    SendKtlsDirect, RecvKtlsDirect
                };
                RunPingPong(cfg, serverConn.m_desc.m_fd, clientConn.m_desc.m_fd);
            }
            else
            {
                printf("  Full kTLS NOT active — falling back to SSL_read for RX\n");

                printf("  %8s  %10s\n", "Size", "Time(us)");
                for (int msgSize : {64, 256, 1024, 4096, 16384})
                {
                    std::vector<char> msg(msgSize, 'A');
                    std::vector<char> buf(msgSize);

                    bool done = false;
                    std::thread serverThread([&]()
                    {
                        char rbuf[16384];
                        while (!done)
                        {
                            int n = RecvPosixPoll(serverConn, rbuf, sizeof(rbuf));
                            if (n <= 0) break;
                            SendKtlsDirect(serverConn.m_desc.m_fd, rbuf, n);
                        }
                    });

                    SendKtlsDirect(clientConn.m_desc.m_fd, msg.data(), msgSize);
                    int remaining = msgSize;
                    while (remaining > 0)
                    {
                        int n = RecvPosixPoll(clientConn, buf.data(), remaining);
                        assert(n > 0);
                        remaining -= n;
                    }

                    constexpr int ITERS = 500;
                    auto t0 = Clock::now();
                    for (int i = 0; i < ITERS; i++)
                    {
                        SendKtlsDirect(clientConn.m_desc.m_fd, msg.data(), msgSize);
                        remaining = msgSize;
                        while (remaining > 0)
                        {
                            int n = RecvPosixPoll(clientConn, buf.data(), remaining);
                            assert(n > 0);
                            remaining -= n;
                        }
                    }
                    auto t1 = Clock::now();
                    double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / ITERS;
                    printf("  %8d  %10.1f\n", msgSize, us);

                    done = true;
                    SendKtlsDirect(clientConn.m_desc.m_fd, msg.data(), 1);
                    serverThread.join();
                }
            }
        }

        // =============================================
        // Test 4: kTLS TLS 1.2 full + TCP_NODELAY
        // =============================================
        printf("\n=== kTLS TLS 1.2 full TX+RX + TCP_NODELAY ===\n");
        {
            io::ssl::Context serverCtx(io::ssl::Mode::Server);
            io::ssl::Context clientCtx(io::ssl::Mode::Client);
            serverCtx.LoadCertificate(certBuf, certLen);
            serverCtx.LoadPrivateKey(keyBuf, keyLen);
            serverCtx.EnableKTLS();
            clientCtx.EnableKTLS();
            SSL_CTX_set_max_proto_version(serverCtx.m_ctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(clientCtx.m_ctx, TLS1_2_VERSION);

            int fds[2];
            MakeTcpPair(fds, true); // TCP_NODELAY
            io::Descriptor serverDesc(fds[0]);
            io::Descriptor clientDesc(fds[1]);
            io::ssl::Connection serverConn(serverCtx, serverDesc, io::ssl::SocketBio{});
            io::ssl::Connection clientConn(clientCtx, clientDesc, io::ssl::SocketBio{});

            bool serverReady = false;
            ctx->GetCooperator()->Spawn({.priority = 0, .stackSize = 65536}, [&](Context*)
            {
                [[maybe_unused]] int r = serverConn.Handshake();
                assert(r == 0);
                serverReady = true;
            });
            [[maybe_unused]] int r = clientConn.Handshake();
            assert(r == 0);
            while (!serverReady) ctx->Yield(true);

            printf("  Server ktls_tx=%d rx=%d, Client ktls_tx=%d rx=%d\n",
                serverConn.m_ktlsTx, serverConn.m_ktlsRx,
                clientConn.m_ktlsTx, clientConn.m_ktlsRx);

            bool fullKtls = serverConn.m_ktlsTx && serverConn.m_ktlsRx
                         && clientConn.m_ktlsTx && clientConn.m_ktlsRx;

            if (fullKtls)
            {
                PingPongConfig cfg = {
                    "ktls-nodelay",
                    SendKtlsDirect, RecvKtlsDirect,
                    SendKtlsDirect, RecvKtlsDirect
                };
                RunPingPong(cfg, serverConn.m_desc.m_fd, clientConn.m_desc.m_fd);
            }
            else
            {
                printf("  Full kTLS NOT active\n");
            }
        }

        // =============================================
        // Test 5: Plaintext + TCP_NODELAY (control)
        // =============================================
        printf("\n=== Plaintext + TCP_NODELAY (control) ===\n");
        {
            int fds[2];
            MakeTcpPair(fds, true);
            PingPongConfig cfg = {
                "plain-nd", SendPlaintext, RecvPlaintext, SendPlaintext, RecvPlaintext
            };
            RunPingPong(cfg, fds[0], fds[1]);
            close(fds[0]);
            close(fds[1]);
        }

        printf("\n");
        ctx->GetCooperator()->Shutdown();
    }, nullptr, {.priority = 0, .stackSize = 65536});

    return 0;
}
