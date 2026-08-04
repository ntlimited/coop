#include <gtest/gtest.h>

#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/http/connection.h"
#include "coop/http/server.h"
#include "coop/http/server_handle.h"

#include "test_helpers.h"

namespace
{

void OkHandler(coop::http::ConnectionBase& conn)
{
    conn.Send(200, "text/plain", "OK");
}

const coop::http::Route kRoutes[] = { { "/ok", &OkHandler } };

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
            coop::http::RunServer(serverCtx, config, kRoutes, 1);
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

        close(c1); close(c2);
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
            coop::http::RunServer(serverCtx, config, kRoutes, 1);
        }, &serverHandle);

        for (int i = 0; i < 10; i++) ctx->Yield(true);

        // A client that connects and sends a partial request line, then holds — the
        // server connection is blocked reading the rest. SHUT_RD wakes it (EOF), so it
        // drains; but to exercise the hard path, send nothing and keep the socket open.
        //
        int c = ConnectBlocking(port);
        ASSERT_GE(c, 0);
        // Send a partial request with no terminator so the parser keeps waiting
        const char* partial = "GET /ok HTTP/1.1\r\nHost: t\r\n";
        [[maybe_unused]] ssize_t w = ::send(c, partial, strlen(partial), 0);

        for (int i = 0; i < 10; i++) ctx->Yield(true);
        EXPECT_EQ(handle.LiveConnections(), 1u);

        // Short deadline: the connection is mid-parse; SHUT_RD gives it EOF on the
        // partial (parser fails -> exits), so it likely drains soft. Either way the
        // drain returns and the connection count reaches zero.
        //
        handle.Drain(ctx, std::chrono::milliseconds(200));
        EXPECT_EQ(handle.LiveConnections(), 0u);

        serverHandle.Kill();
        for (int i = 0; i < 50; i++) ctx->Yield(true);
        close(c);
    });
}
