// Directional hot-loopback comparison of HTTP file-send paths.
// Build in Release and run from a directory containing cert.pem and key.pem.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include <openssl/ssl.h>
#include "coop/alloc.h"
#include "coop/cooperator.h"
#include "coop/countdown_latch.h"
#include "coop/http/connection.h"
#include "coop/http/tls_transport.h"
#include "coop/http/transport.h"
#include "coop/io/io.h"
#include "coop/io/read_file.h"
#include "coop/io/read.h"
#include "coop/io/write.h"
#include "coop/io/ssl/ssl.h"
#include "coop/self.h"
#include "coop/thread.h"
#include "coop/time/now.h"

static void Check(bool condition) { if (!condition) std::abort(); }
static int64_t CpuUs()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec * 1000000LL + usage.ru_utime.tv_usec
         + usage.ru_stime.tv_sec * 1000000LL + usage.ru_stime.tv_usec;
}

struct Pair
{
    coop::io::Descriptor listener{socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)};
    coop::io::Descriptor client{socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)};
    coop::io::Descriptor server{coop::io::borrowed, -1};
    coop::io::ssl::Context clientContext{coop::io::ssl::Mode::Client};
    coop::io::ssl::Context serverContext{coop::io::ssl::Mode::Server};
    char clientBuffer[16384], serverBuffer[16384];
    std::unique_ptr<coop::io::ssl::Connection> clientTls, serverTls;
    explicit Pair(int kind)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(listener.m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        Check(listen(listener.m_fd, 1) == 0);
        socklen_t len = sizeof(addr);
        Check(getsockname(listener.m_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        Check(coop::io::ConnectKill(client, reinterpret_cast<sockaddr*>(&addr), len,
                                  std::chrono::seconds(3)) == 0);
        server.m_fd = coop::io::AcceptKill(listener, nullptr, nullptr, SOCK_NONBLOCK,
                                          std::chrono::seconds(3));
        Check(server.m_fd >= 0);
        server.m_owned = true;
        // Match the HTTP server's accepted-socket policy for every transport.
        int noDelay = 1;
        Check(setsockopt(server.m_fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay)) == 0);
        Check(setsockopt(client.m_fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay)) == 0);
        if (kind == 0) return;
        char cert[8192], key[8192];
        int certLen = coop::io::ReadFile("cert.pem", cert, sizeof(cert));
        int keyLen = coop::io::ReadFile("key.pem", key, sizeof(key));
        Check(certLen > 0 && keyLen > 0);
        Check(serverContext.LoadCertificate(cert, certLen));
        Check(serverContext.LoadPrivateKey(key, keyLen));
        if (kind == 2)
        {
            clientContext.EnableKTLS();
            serverContext.EnableKTLS();
            Check(SSL_CTX_set_max_proto_version(clientContext.m_ctx, TLS1_2_VERSION) == 1);
            Check(SSL_CTX_set_max_proto_version(serverContext.m_ctx, TLS1_2_VERSION) == 1);
            Check(SSL_CTX_set_cipher_list(clientContext.m_ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256") == 1);
            Check(SSL_CTX_set_cipher_list(serverContext.m_ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256") == 1);
            clientTls = std::make_unique<coop::io::ssl::Connection>(
                clientContext, client, coop::io::ssl::SocketBio{});
            serverTls = std::make_unique<coop::io::ssl::Connection>(
                serverContext, server, coop::io::ssl::SocketBio{});
        }
        else
        {
            clientTls = std::make_unique<coop::io::ssl::Connection>(
                clientContext, client, clientBuffer, sizeof(clientBuffer));
            serverTls = std::make_unique<coop::io::ssl::Connection>(
                serverContext, server, serverBuffer, sizeof(serverBuffer));
        }
        coop::CountdownLatch joined(1);
        Check(coop::Spawn({.stackSize = 65536}, [&](coop::Context*)
        {
            Check(serverTls->HandshakeKill() == 0);
            joined.Arrive();
        }));
        Check(clientTls->HandshakeKill() == 0);
        joined.Wait(coop::Self());
    }
};

template<typename Transport>
static void Transfer(Pair& pair, Transport transport, int fd, size_t bytes, bool baseline)
{
    coop::CountdownLatch joined(1);
    Check(coop::Spawn({.stackSize = 65536}, [&](coop::Context*)
    {
        char buffer[16384];
        size_t received = 0, headerSize = 0;
        char header[1024];
        bool headersComplete = false;
        while (received < bytes)
        {
            int n = pair.clientTls
                ? coop::io::ssl::RecvKill(*pair.clientTls, buffer, sizeof(buffer), std::chrono::seconds(3))
                : coop::io::RecvKill(pair.client, buffer, sizeof(buffer), 0, std::chrono::seconds(3));
            Check(n > 0);
            size_t at = 0;
            while (!headersComplete && at < static_cast<size_t>(n))
            {
                Check(headerSize < sizeof(header));
                header[headerSize++] = buffer[at++];
                headersComplete = headerSize >= 4
                    && std::memcmp(header + headerSize - 4, "\r\n\r\n", 4) == 0;
            }
            received += static_cast<size_t>(n) - at;
        }
        Check(headersComplete && received == bytes);
        std::string parsed(header, headerSize);
        Check(parsed.starts_with("HTTP/1.1 200 OK\r\n"));
        Check(parsed.find("Content-Length: " + std::to_string(bytes) + "\r\n") != std::string::npos);
        joined.Arrive();
    }));
    using Connection = coop::http::Connection<Transport>;
    auto conn = coop::Self()->Allocate<Connection>(Connection::ExtraBytes(), transport,
        coop::Self(), coop::Self()->GetCooperator());
    Check(conn->SendHeaders(200, "application/octet-stream", bytes));
    if (baseline)
    {
        // Reconstruct the former 4 KiB copy loop without Connection::Sendfile fairness.
        char buffer[4096];
        coop::io::Descriptor file(coop::io::borrowed, fd);
        size_t at = 0;
        while (at < bytes)
        {
            int n = coop::io::ReadKill(file, buffer, std::min(bytes - at, sizeof(buffer)), at);
            Check(n > 0);
            size_t sent = 0;
            while (sent < static_cast<size_t>(n))
            {
                int result = transport.Send(buffer + sent, n - sent, std::chrono::seconds(3));
                Check(result > 0);
                sent += static_cast<size_t>(result);
            }
            at += static_cast<size_t>(n);
        }
    }
    else
    {
        Check(conn->Sendfile(fd, 0, bytes));
    }
    joined.Wait(coop::Self());
}

int main(int argc, char** argv)
{
    const int repetitions = argc > 1 ? std::atoi(argv[1]) : 7;
    Check(repetitions > 0 && repetitions <= 1000);
    const size_t bytes = 32 * 1024 * 1024;
    int status = EXIT_SUCCESS;
    coop::Cooperator co;
    {
        coop::Thread thread(&co);
        co.Submit([&](coop::Context* ctx)
        {
            ctx->SetDeadlineIn(std::chrono::seconds(180));
            coop::io::Descriptor file(memfd_create("http-file-bench", MFD_CLOEXEC));
            Check(file.m_fd >= 0);
            char buffer[16384]{};
            for (size_t at = 0; at < bytes; at += sizeof(buffer))
                Check(coop::io::WriteKill(file, buffer, sizeof(buffer), at) == sizeof(buffer));
            for (int kind = 0; kind < 3; ++kind)
            {
                auto pair = std::make_unique<Pair>(kind);
                if (kind == 2 && (!pair->serverTls->m_ktlsTx || !pair->clientTls->m_ktlsRx))
                {
                    std::fputs("kTLS required: kernel/OpenSSL did not activate TX/RX\n", stderr);
                    status = EXIT_FAILURE;
                    break;
                }
                std::printf("transport=%s ktls_tx=%d ktls_rx=%d bytes=%zu\n",
                    kind == 0 ? "plaintext" : kind == 1 ? "memory_tls" : "ktls",
                    pair->serverTls && pair->serverTls->m_ktlsTx,
                    pair->clientTls && pair->clientTls->m_ktlsRx, bytes);
                for (int repetition = 0; repetition < repetitions; ++repetition)
                    for (int order = 0; order < 2; ++order)
                    {
                        bool baseline = (repetition + order) % 2 == 0;
                        int64_t cpu = CpuUs(), start = coop::time::MonotonicMicros();
                        if (kind == 0)
                            Transfer(*pair, coop::http::PlaintextTransport(pair->server), file.m_fd, bytes, baseline);
                        else
                            Transfer(*pair, coop::http::TlsTransport(*pair->serverTls, pair->server), file.m_fd, bytes, baseline);
                        std::printf("sample=%d path=%s elapsed_us=%lld cpu_us=%lld\n", repetition,
                            baseline ? "before_4k_copy" : "after",
                            static_cast<long long>(coop::time::MonotonicMicros() - start),
                            static_cast<long long>(CpuUs() - cpu));
                    }
            }
            ctx->GetCooperator()->Shutdown();
        }, {.stackSize = 131072});
    }
    return status;
}
