#include <gtest/gtest.h>

#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <memory>
#include <optional>

#include <openssl/x509.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/io/ssl/context.h"
#include "coop/io/ssl/connection.h"
#include "coop/io/ssl/send.h"
#include "coop/io/ssl/recv.h"
#include "coop/http/connection.h"
#include "coop/http/server.h"
#include "coop/http/server_handle.h"

#include "test_helpers.h"

namespace
{

void OkHandler(coop::http::ConnectionBase& conn, void*)
{
    conn.Send(200, "text/plain", "OK");
}

int ConnectBlocking(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

} // end anonymous namespace

// Full graceful-drain lifecycle: keep-alive connections in flight, Drain stops accept,
// in-flight connections get Connection: close and finish, and an idle keep-alive
// connection is woken (SHUT_RD -> EOF) so the drain completes cleanly within the window.
//
TEST(DrainTest, GracefulDrainClosesKeepAlive)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int port = 44000 + (getpid() % 20000);
        coop::http::ServerHandle handle;

        coop::Context::Handle serverHandle;
        bool serverReturned = false;
        ctx->GetCooperator()->Spawn(
            {.priority = 0, .stackSize = 65536},
            [&, port](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = port;
            config.control = &handle;
            config.name = "DrainServer";
            config.handler = &OkHandler;
            coop::http::RunServer(serverCtx, config);
            serverReturned = true;
        }, &serverHandle);

        for (int i = 0; i < 10; i++) ctx->Yield(true);

        // Two keep-alive clients, each having done one request (so they're idle,
        // blocked awaiting the next request).
        //
        auto doRequest = [&](int fd, coop::io::Descriptor& d)
        {
            const char* req = "GET /ok HTTP/1.1\r\nHost: t\r\n\r\n";
            coop::io::SendAll(d, req, strlen(req));
            char buf[256] = {};
            int n = coop::io::Recv(d, buf, sizeof(buf), 0, std::chrono::seconds(1));
            return std::string(buf, n > 0 ? n : 0);
        };

        int c1 = ConnectBlocking(port);
        int c2 = ConnectBlocking(port);
        ASSERT_GE(c1, 0); ASSERT_GE(c2, 0);
        coop::io::Descriptor d1(c1, coop::GetUring());
        coop::io::Descriptor d2(c2, coop::GetUring());

        std::string r1 = doRequest(c1, d1);
        EXPECT_NE(r1.find("200 OK"), std::string::npos);
        EXPECT_NE(r1.find("keep-alive"), std::string::npos);   // still keep-alive pre-drain
        std::string r2 = doRequest(c2, d2);
        EXPECT_NE(r2.find("200 OK"), std::string::npos);

        for (int i = 0; i < 5; i++) ctx->Yield(true);
        EXPECT_EQ(handle.LiveConnections(), 2u);

        // Drain: idle keep-alive connections are woken (SHUT_RD -> clean EOF) and exit;
        // the drain completes without needing the hard kill.
        //
        bool clean = handle.Drain(ctx, std::chrono::seconds(2));
        EXPECT_TRUE(clean);
        EXPECT_EQ(handle.LiveConnections(), 0u);

        // Accept is stopped: a new connection is refused / immediately closed.
        //
        int c3 = ConnectBlocking(port);
        if (c3 >= 0)
        {
            coop::io::Descriptor d3(c3, coop::GetUring());
            const char* req = "GET /ok HTTP/1.1\r\nHost: t\r\n\r\n";
            coop::io::SendAll(d3, req, strlen(req));
            char buf[64] = {};
            int n = coop::io::Recv(d3, buf, sizeof(buf), 0, std::chrono::milliseconds(300));
            EXPECT_LE(n, 0);   // no response — server not accepting
        }

        // The server context unwinds once its accept loop breaks (listener shut).
        //
        for (int i = 0; i < 50 && !serverReturned; i++) ctx->Yield(true);
        EXPECT_TRUE(serverReturned);

    });
}

// A connection that never closes (client holds it, mid-request stuck) must be
// hard-killed at the deadline.
//
TEST(DrainTest, HardDeadlineKillsStragglers)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int port = 45000 + (getpid() % 20000);
        coop::http::ServerHandle handle;

        coop::Context::Handle serverHandle;
        ctx->GetCooperator()->Spawn(
            {.priority = 0, .stackSize = 65536},
            [&, port](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = port;
            config.control = &handle;
            config.handler = &OkHandler;
            coop::http::RunServer(serverCtx, config);
        }, &serverHandle);

        for (int i = 0; i < 10; i++) ctx->Yield(true);

        // A client that connects and sends a partial request line, then holds — the
        // server connection is blocked reading the rest. A complete request line is
        // active work, so the soft phase preserves reads until the hard deadline.
        //
        int c = ConnectBlocking(port);
        ASSERT_GE(c, 0);
        // Send a partial request with no terminator so the parser keeps waiting
        const char* partial = "GET /ok HTTP/1.1\r\nHost: t\r\n";
        [[maybe_unused]] ssize_t w = ::send(c, partial, strlen(partial), 0);

        for (int i = 0; i < 10; i++) ctx->Yield(true);
        EXPECT_EQ(handle.LiveConnections(), 1u);

        // The hard phase terminates the incomplete headers; connection teardown must
        // finish before Drain returns.
        //
        handle.Drain(ctx, std::chrono::milliseconds(200));
        EXPECT_EQ(handle.LiveConnections(), 0u);

        serverHandle.Kill();
        for (int i = 0; i < 50; i++) ctx->Yield(true);
        close(c);
    });
}

namespace
{

// OpenSSL's handshake stack is larger than the default test context stack. Construct
// certificates outside the cooperator and run TLS client operations on an explicit stack.
//
void RunDrainTest(std::function<void(coop::Context*)> fn)
{
    coop::Cooperator co;
    coop::Thread thread(&co);
    co.Submit([&](coop::Context* ctx)
    {
        fn(ctx);
        co.Shutdown();
    }, {.stackSize = 131072});
}

class DrainProtocolTest : public testing::TestWithParam<bool>
{
  protected:
    coop::io::ssl::Context m_tls{coop::io::ssl::Mode::Server};

    void SetUp() override
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        ASSERT_NE(generator, nullptr);
        ASSERT_EQ(EVP_PKEY_keygen_init(generator.get()), 1);
        ASSERT_EQ(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(),
            NID_X9_62_prime256v1), 1);
        EVP_PKEY* rawKey = nullptr;
        ASSERT_EQ(EVP_PKEY_keygen(generator.get(), &rawKey), 1);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        ASSERT_NE(cert, nullptr);
        ASSERT_EQ(X509_set_version(cert.get(), 2), 1);
        ASSERT_EQ(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1), 1);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600), nullptr);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400), nullptr);
        ASSERT_EQ(X509_set_pubkey(cert.get(), key.get()), 1);
        X509_NAME* name = X509_get_subject_name(cert.get());
        ASSERT_EQ(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0), 1);
        ASSERT_EQ(X509_set_issuer_name(cert.get(), name), 1);
        ASSERT_GT(X509_sign(cert.get(), key.get(), EVP_sha256()), 0);
        ASSERT_EQ(SSL_CTX_use_certificate(m_tls.m_ctx, cert.get()), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey(m_tls.m_ctx, key.get()), 1);
    }
};

struct DrainServer
{
    coop::Context* context;
    coop::http::ServerHandle control;
    coop::http::ServerConfiguration config;
    coop::Context::Handle task;
    bool returned = false;

    DrainServer(coop::Context* ctx, coop::io::ssl::Context* tls,
                coop::http::RequestHandler handler = &OkHandler, void* state = nullptr)
        : context(ctx)
    {
        // Reserve an ephemeral loopback port, then let RunServer bind it. Each test
        // owns its port; unlike the older fixed ranges this works with parallel ctest.
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_GE(probe, 0);
        EXPECT_EQ(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        socklen_t length = sizeof(address);
        EXPECT_EQ(getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length), 0);
        config.port = ntohs(address.sin_port);
        close(probe);
        config.reusePort = false;
        config.control = &control;
        config.handler = handler;
        config.userData = state;
        ctx->GetCooperator()->Spawn({.stackSize = 65536}, [this, tls](coop::Context* server)
        {
            EXPECT_TRUE(tls ? coop::http::RunTlsServer(server, config, *tls)
                            : coop::http::RunServer(server, config));
            returned = true;
        }, &task);
        while (!control.HasListener() && !returned) ctx->Yield(true);
        EXPECT_TRUE(control.HasListener());
    }

    ~DrainServer()
    {
        // Keep state alive through detached connection and listener teardown even when
        // a fatal assertion returns from the test's body.
        if (control.LiveConnections()) control.Drain(context, std::chrono::milliseconds(10));
        if (task) task.Kill();
        while (task) context->Yield(true);
    }

    void AwaitStopped()
    {
        while (task) context->Yield(true);
        EXPECT_TRUE(returned);
        EXPECT_FALSE(control.HasListener());
    }
};

struct DrainClient
{
    coop::io::Descriptor descriptor;
    coop::io::ssl::Context context{coop::io::ssl::Mode::Client};
    std::optional<coop::io::ssl::Connection> tls;

    DrainClient(int port, bool encrypted) : descriptor(ConnectBlocking(port))
    {
        EXPECT_GE(descriptor.m_fd, 0);
        fcntl(descriptor.m_fd, F_SETFL, fcntl(descriptor.m_fd, F_GETFL) | O_NONBLOCK);
        if (encrypted)
        {
            tls.emplace(context, descriptor, coop::io::ssl::SocketBio{});
            EXPECT_EQ(tls->HandshakeKill(), 0);
        }
    }

    int Send(std::string_view data)
    {
        return tls ? coop::io::ssl::SendAllKill(*tls, data.data(), data.size())
                   : coop::io::SendAll(descriptor, data.data(), data.size());
    }

    std::string Response()
    {
        std::string response;
        while (response.find("\r\n\r\nOK") == std::string::npos)
        {
            char bytes[512];
            int count = tls ? coop::io::ssl::RecvKill(*tls, bytes, sizeof(bytes))
                            : coop::io::Recv(descriptor, bytes, sizeof(bytes), 0,
                                             std::chrono::seconds(2));
            if (count <= 0) break;
            response.append(bytes, count);
        }
        return response;
    }
};

TEST_P(DrainProtocolTest, IdleKeepAliveDrainsAndClearsListener)
{
    RunDrainTest([&](coop::Context* ctx)
    {
        DrainServer server(ctx, GetParam() ? &m_tls : nullptr);
        DrainClient client(server.config.port, GetParam());
        for (int i = 0; i < 2; ++i)
        {
            ASSERT_GT(client.Send("GET / HTTP/1.1\r\nHost: t\r\n\r\n"), 0);
            auto response = client.Response();
            EXPECT_NE(response.find("200 OK"), std::string::npos);
            EXPECT_NE(response.find("keep-alive"), std::string::npos);
        }
        EXPECT_EQ(server.control.LiveConnections(), 1u);
        EXPECT_TRUE(server.control.Drain(ctx, std::chrono::seconds(2)));
        EXPECT_EQ(server.control.LiveConnections(), 0u);
        server.AwaitStopped();
        EXPECT_TRUE(server.control.Drain(ctx, std::chrono::milliseconds(1)));
    });
}

TEST_P(DrainProtocolTest, SoftDrainPreservesActiveUpload)
{
    RunDrainTest([&](coop::Context* ctx)
    {
        struct Upload
        {
            coop::Coordinator entered;
            std::string bytes;
            bool complete = false;
        } upload;
        upload.entered.TryAcquire(ctx);
        DrainServer server(ctx, GetParam() ? &m_tls : nullptr,
            [](coop::http::ConnectionBase& conn, void* state)
        {
            auto& upload = *static_cast<Upload*>(state);
            upload.entered.Release(coop::Self(), false);
            while (auto part = conn.NextBody())
            {
                upload.bytes.append(static_cast<const char*>(part->data), part->size);
            }
            upload.complete = conn.Complete();
            conn.Send(200, "text/plain", "OK");
        }, &upload);
        DrainClient client(server.config.port, GetParam());
        ASSERT_GT(client.Send("POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\n"), 0);
        upload.entered.Acquire(ctx);
        upload.entered.Release(ctx, false);
        coop::Coordinator finished;
        finished.TryAcquire(ctx);
        bool clean = false;
        ctx->GetCooperator()->Spawn([&](coop::Context* draining)
        {
            clean = server.control.Drain(draining, std::chrono::seconds(2));
            finished.Release(draining, false);
        });
        while (!server.control.IsDraining()) ctx->Yield(true);
        EXPECT_EQ(server.control.LiveConnections(), 1u);
        EXPECT_GT(client.Send("data"), 0);
        auto response = client.Response();
        finished.Acquire(ctx);
        finished.Release(ctx, false);
        EXPECT_TRUE(clean);
        EXPECT_TRUE(upload.complete);
        EXPECT_EQ(upload.bytes, "data");
        EXPECT_NE(response.find("200 OK"), std::string::npos);
        EXPECT_NE(response.find("Connection: close"), std::string::npos);
        EXPECT_EQ(server.control.LiveConnections(), 0u);
        server.AwaitStopped();
    });
}

TEST_P(DrainProtocolTest, AcceptorExitClearsBorrowedListener)
{
    RunDrainTest([&](coop::Context* ctx)
    {
        DrainServer server(ctx, GetParam() ? &m_tls : nullptr);
        server.task.Kill();
        server.AwaitStopped();
        // A subsequent drain must not dereference the listener formerly on its stack.
        EXPECT_TRUE(server.control.Drain(ctx, std::chrono::milliseconds(1)));
    });
}

TEST_P(DrainProtocolTest, BlockedRequestOrHandshakeNeedsHardDeadline)
{
    RunDrainTest([&](coop::Context* ctx)
    {
        DrainServer server(ctx, GetParam() ? &m_tls : nullptr);
        coop::io::Descriptor client(ConnectBlocking(server.config.port));
        ASSERT_GE(client.m_fd, 0);
        if (!GetParam())
        {
            // A complete request line transfers ownership to the active handler/body
            // drain; incomplete headers then keep its read pending until the deadline.
            const char request[] = "GET / HTTP/1.1\r\nHost: t\r\n";
            ASSERT_GT(coop::io::SendAll(client, request, sizeof(request) - 1), 0);
        }
        // TLS sends no ClientHello. It must already be registered and detached while
        // HandshakeKill waits, otherwise Drain reports zero and returns prematurely.
        for (int i = 0; i < 100; ++i) ctx->Yield(true);
        EXPECT_EQ(server.control.LiveConnections(), 1u);
        EXPECT_FALSE(server.control.Drain(ctx, std::chrono::milliseconds(20)));
        EXPECT_EQ(server.control.LiveConnections(), 0u);
        server.AwaitStopped();
    });
}

INSTANTIATE_TEST_SUITE_P(HttpAndTls, DrainProtocolTest, testing::Bool(),
    [](const testing::TestParamInfo<bool>& info) { return info.param ? "Tls" : "Plaintext"; });

} // namespace
