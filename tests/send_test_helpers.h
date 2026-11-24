#pragma once

#include <algorithm>
#include <csignal>
#include <pthread.h>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "coop/countdown_latch.h"
#include "coop/http/tls_transport.h"
#include "coop/http/transport.h"
#include "coop/io/shutdown.h"
#include "coop/io/accept.h"
#include "coop/io/connect.h"
#include "coop/io/ssl/context.h"
#include "coop/io/ssl/recv.h"
#include "coop/time/now.h"
#include "coop/time/sleep.h"
#include "test_helpers.h"

namespace test
{

struct ScopedSigpipeBlock
{
    sigset_t signals, saved;
    bool alreadyPending = false;
    ScopedSigpipeBlock()
    {
        sigemptyset(&signals);
        sigaddset(&signals, SIGPIPE);
        sigset_t pending;
        EXPECT_EQ(sigpending(&pending), 0);
        alreadyPending = sigismember(&pending, SIGPIPE) == 1;
        EXPECT_EQ(pthread_sigmask(SIG_BLOCK, &signals, &saved), 0);
    }
    ~ScopedSigpipeBlock()
    {
        if (!alreadyPending && sigismember(&saved, SIGPIPE) != 1)
        {
            timespec immediate{};
            // Standard signals coalesce, so consume at most the signal generated here.
            sigtimedwait(&signals, nullptr, &immediate);
        }
        EXPECT_EQ(pthread_sigmask(SIG_SETMASK, &saved, nullptr), 0);
    }
};

enum class SendTransport { Plaintext, MemoryTls, SocketTls, Ktls };
enum class SendScenario { Stalled, Steady, KilledFlush, Deadline, FinalFlush, CloseFlush, KilledFile, TrickleDeadline, KilledSend };

struct SendPair
{
    int fds[2];
    coop::io::Descriptor client;
    coop::io::Descriptor server;
    coop::io::ssl::Context clientContext{coop::io::ssl::Mode::Client};
    coop::io::ssl::Context serverContext{coop::io::ssl::Mode::Server};
    char clientBuffer[16384];
    char serverBuffer[16384];
    std::unique_ptr<coop::io::ssl::Connection> clientTls;
    std::unique_ptr<coop::io::ssl::Connection> serverTls;

    explicit SendPair(SendTransport kind)
    : fds{-1, -1}
    , client(coop::io::borrowed, -1)
    , server(coop::io::borrowed, -1)
    {
        if (kind == SendTransport::Ktls)
        {
            coop::io::Descriptor listener(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            EXPECT_EQ(bind(listener.m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
            EXPECT_EQ(listen(listener.m_fd, 1), 0);
            socklen_t length = sizeof(address);
            EXPECT_EQ(getsockname(listener.m_fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
            fds[0] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            client.m_fd = fds[0];
            client.m_owned = true;
            int receiveCapacity = 4096;
            EXPECT_EQ(setsockopt(client.m_fd, SOL_SOCKET, SO_RCVBUF,
                                &receiveCapacity, sizeof(receiveCapacity)), 0);
            EXPECT_EQ(coop::io::ConnectKill(client, reinterpret_cast<sockaddr*>(&address),
                                           length, std::chrono::seconds(1)), 0);
            fds[1] = coop::io::AcceptKill(listener, nullptr, nullptr, SOCK_NONBLOCK,
                                         std::chrono::seconds(1));
            EXPECT_GE(fds[1], 0);
        }
        else EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);
        client.m_fd = fds[0];
        client.m_owned = true;
        server.m_fd = fds[1];
        server.m_owned = true;
        int size = 4096;
        EXPECT_EQ(setsockopt(server.m_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
        if (kind == SendTransport::Plaintext) return;
        MakeCertificate();
        if (kind == SendTransport::Ktls)
        {
            clientContext.EnableKTLS();
            serverContext.EnableKTLS();
            EXPECT_EQ(SSL_CTX_set_cipher_list(clientContext.m_ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256"), 1);
            EXPECT_EQ(SSL_CTX_set_cipher_list(serverContext.m_ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256"), 1);
        }
        if (kind == SendTransport::SocketTls || kind == SendTransport::Ktls)
        {
            clientTls = std::make_unique<coop::io::ssl::Connection>(
                clientContext, client, coop::io::ssl::SocketBio{});
            serverTls = std::make_unique<coop::io::ssl::Connection>(
                serverContext, server, coop::io::ssl::SocketBio{});
            coop::CountdownLatch done(1);
            coop::Spawn({.stackSize = 65536}, [&](coop::Context*)
            {
                EXPECT_EQ(serverTls->HandshakeKill(), 0);
                done.Arrive();
            });
            EXPECT_EQ(clientTls->HandshakeKill(), 0);
            EXPECT_EQ(done.WaitKill(coop::Self()),
                      coop::CountdownLatch::WaitStatus::Completed);
        }
        else
        {
            clientTls = std::make_unique<coop::io::ssl::Connection>(
                clientContext, client, clientBuffer, sizeof(clientBuffer));
            serverTls = std::make_unique<coop::io::ssl::Connection>(
                serverContext, server, serverBuffer, sizeof(serverBuffer));
            for (int step = 0; step < 100; ++step)
            {
                SSL_do_handshake(clientTls->m_ssl);
                Transfer(clientTls->m_ssl, serverTls->m_ssl);
                SSL_do_handshake(serverTls->m_ssl);
                Transfer(serverTls->m_ssl, clientTls->m_ssl);
                if (SSL_is_init_finished(clientTls->m_ssl)
                    && SSL_is_init_finished(serverTls->m_ssl)) break;
            }
            EXPECT_TRUE(SSL_is_init_finished(clientTls->m_ssl));
            EXPECT_TRUE(SSL_is_init_finished(serverTls->m_ssl));
        }
    }

    void MakeCertificate()
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        ASSERT_NE(generator, nullptr);
        ASSERT_EQ(EVP_PKEY_keygen_init(generator.get()), 1);
        ASSERT_EQ(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(),
                                                       NID_X9_62_prime256v1), 1);
        EVP_PKEY* raw = nullptr;
        ASSERT_EQ(EVP_PKEY_keygen(generator.get(), &raw), 1);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        ASSERT_NE(cert, nullptr);
        ASSERT_EQ(X509_set_version(cert.get(), 2), 1);
        ASSERT_EQ(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1), 1);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60), nullptr);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400), nullptr);
        ASSERT_EQ(X509_set_pubkey(cert.get(), key.get()), 1);
        auto* name = X509_get_subject_name(cert.get());
        ASSERT_EQ(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0), 1);
        ASSERT_EQ(X509_set_issuer_name(cert.get(), name), 1);
        ASSERT_GT(X509_sign(cert.get(), key.get(), EVP_sha256()), 0);
        ASSERT_EQ(SSL_CTX_use_certificate(serverContext.m_ctx, cert.get()), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey(serverContext.m_ctx, key.get()), 1);
        ASSERT_EQ(SSL_CTX_set_max_proto_version(serverContext.m_ctx, TLS1_2_VERSION), 1);
        ASSERT_EQ(SSL_CTX_set_max_proto_version(clientContext.m_ctx, TLS1_2_VERSION), 1);
    }

    static void Transfer(SSL* from, SSL* to)
    {
        char buffer[4096];
        int n;
        while ((n = BIO_read(SSL_get_wbio(from), buffer, sizeof(buffer))) > 0)
            EXPECT_EQ(BIO_write(SSL_get_rbio(to), buffer, n), n);
    }

    int Recv(void* data, size_t size)
    {
        return clientTls
            ? coop::io::ssl::RecvKill(*clientTls, data, size, std::chrono::milliseconds(150))
            : coop::io::RecvKill(client, data, size, 0, std::chrono::milliseconds(150));
    }

    void FillSocket()
    {
        char buffer[4096]{};
        size_t total = 0;
        while (total < 1024 * 1024)
        {
            ssize_t n = send(server.m_fd, buffer, sizeof(buffer), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0)
            {
                EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
                return;
            }
            ASSERT_GT(n, 0);
            total += static_cast<size_t>(n);
        }
        FAIL() << "socket did not fill within the test bound";
    }
};

// Blocked sends are rescued after one second (two seconds for paced transfers), so
// failures remain assertions rather than waiting for a peer that will never read.
//
template<typename Send>
void RunSendScenario(SendTransport kind, SendScenario scenario, Send sendResponse)
{
    RunInCooperator([&](coop::Context* ctx)
    {
        // The rescue can close a socket while an old TLS writer is active.
        ScopedSigpipeBlock blockedSigpipe;
        ctx->SetDeadlineIn(std::chrono::seconds(3));
        auto pair = std::make_unique<SendPair>(kind);
        const bool flush = scenario == SendScenario::KilledFlush
            || scenario == SendScenario::FinalFlush || scenario == SendScenario::CloseFlush;
        if (flush) pair->FillSocket();
        size_t payloadSize = (scenario == SendScenario::Steady || scenario == SendScenario::TrickleDeadline) ? 512 * 1024 : 64 * 1024;
        std::string payload(flush ? 7 : payloadSize, 'x');
        coop::CountdownLatch joined(3);
        coop::CountdownLatch staged(1);
        coop::Context::Handle sender, rescue;
        bool sent = true, siblingRan = false, finished = false;
        int64_t elapsed = 0;
        coop::KillCause cause = coop::KillCause::None;
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* child)
        {
            if (scenario == SendScenario::Deadline)
                child->SetDeadlineIn(std::chrono::milliseconds(30));
            if (scenario == SendScenario::TrickleDeadline)
                child->SetDeadlineIn(std::chrono::milliseconds(250));
            auto timeout = (scenario == SendScenario::Steady || scenario == SendScenario::TrickleDeadline) ? std::chrono::milliseconds(150)
                : scenario == SendScenario::Deadline
                    || scenario == SendScenario::KilledFlush
                    || scenario == SendScenario::KilledFile
                    || scenario == SendScenario::KilledSend
                ? std::chrono::milliseconds(1000) : std::chrono::milliseconds(60);
            const int64_t start = coop::time::MonotonicMicros();
            if (kind == SendTransport::Plaintext)
            {
                coop::http::PlaintextTransport transport(pair->server);
                sent = sendResponse(transport, child, payload, timeout, flush, staged);
            }
            else
            {
                coop::http::TlsTransport transport(*pair->serverTls, pair->server);
                sent = sendResponse(transport, child, payload, timeout, flush, staged);
            }
            elapsed = coop::time::MonotonicMicros() - start;
            cause = child->WhyKilled();
            finished = true;
            if (rescue) rescue.Kill();
            joined.Arrive();
        }, &sender));
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context*)
        {
            siblingRan = true;
            if (scenario == SendScenario::Steady || scenario == SendScenario::TrickleDeadline)
            {
                // Drain a complete TLS record before pacing. Smaller reads can consume
                // only decrypted cache for several sleeps without any socket progress.
                // The larger payload still takes longer than the inactivity timeout.
                //
                char buffer[16384];
                size_t read = 0;
                while (read < payload.size() && !finished)
                {
                    int n = pair->Recv(buffer, sizeof(buffer));
                    if (n <= 0) break;
                    read += static_cast<size_t>(n);
                    coop::time::Sleep(scenario == SendScenario::TrickleDeadline
                        ? std::chrono::milliseconds(30) : std::chrono::milliseconds(8));
                }
            }
            else if (scenario == SendScenario::KilledFlush || scenario == SendScenario::KilledFile
                     || scenario == SendScenario::KilledSend)
            {
                if (scenario == SendScenario::KilledFile || scenario == SendScenario::KilledSend)
                    EXPECT_EQ(staged.WaitKill(coop::Self()), coop::CountdownLatch::WaitStatus::Completed);
                sender.Kill();
            }
            joined.Arrive();
        }));
        ASSERT_TRUE(coop::Spawn([&](coop::Context*)
        {
            if (!finished)
                coop::time::Sleep((scenario == SendScenario::Steady || scenario == SendScenario::TrickleDeadline)
                    ? std::chrono::milliseconds(2000) : std::chrono::milliseconds(1000));
            if (!finished)
            {
                coop::io::Shutdown(pair->client, SHUT_RDWR);
                pair->client.Close();
            }
            joined.Arrive();
        }, &rescue));
        EXPECT_EQ(joined.WaitKill(ctx), coop::CountdownLatch::WaitStatus::Completed);
        EXPECT_TRUE(siblingRan);
        if (scenario == SendScenario::Steady)
        {
            EXPECT_TRUE(sent);
            EXPECT_GT(elapsed, 150000);
        }
        else
        {
            EXPECT_FALSE(sent);
            EXPECT_LT(elapsed, 800000);
        }
        if (scenario == SendScenario::Deadline || scenario == SendScenario::TrickleDeadline)
            EXPECT_EQ(cause, coop::KillCause::Deadline);
        if (scenario == SendScenario::TrickleDeadline) EXPECT_GT(elapsed, 150000);
        if (scenario == SendScenario::KilledFlush || scenario == SendScenario::KilledFile
            || scenario == SendScenario::KilledSend)
            EXPECT_EQ(cause, coop::KillCause::Kill);
    }, {.stackSize = 65536});
}

struct SendScript
{
    std::string output;
    size_t limit = 11;
    size_t largest = 0;
    size_t calls = 0;
    size_t tryCalls = 0;
    size_t tryLimit = SIZE_MAX;
    bool killOnWrite = false;
    bool cancelAfterWrite = false;
};

struct ShortWriteTransport
{
    static constexpr bool kSpliceable = false;
    SendScript* script;
    coop::io::Descriptor* descriptor;
    coop::io::Descriptor& Descriptor() { return *descriptor; }
    int Recv(void*, size_t, int, coop::time::Interval) { return 0; }
    int Send(const void* data, size_t size, coop::time::Interval)
    {
        ++script->calls;
        if (script->killOnWrite) coop::Self()->KillSelf(coop::KillCause::Kill);
        script->largest = std::max(script->largest, size);
        size_t n = std::min(size, script->limit);
        script->output.append(static_cast<const char*>(data), n);
        return script->cancelAfterWrite ? -ECANCELED : static_cast<int>(n);
    }
    int TrySend(const void* data, size_t size)
    {
        ++script->tryCalls;
        size_t n = std::min(size, script->tryLimit);
        script->output.append(static_cast<const char*>(data), n);
        return static_cast<int>(n);
    }
    int SendAll(const void* data, size_t size) { return Send(data, size, {}); }
    int Sendfile(int, off_t, size_t, coop::time::Interval) { return -ENOTSUP; }
    int SendfileAll(int, off_t, size_t) { return -ENOTSUP; }
};

} // namespace test
