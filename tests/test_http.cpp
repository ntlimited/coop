#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/alloc.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/poll.h"
#include "coop/io/connect.h"
#include "coop/io/send.h"
#include "coop/io/write.h"
#include "coop/http/connection.h"
#include "coop/http/client.h"
#include "coop/http/server.h"
#include "coop/http/server_handle.h"
#include "coop/http/status.h"
#include "coop/coordinate_with.h"
#include "coop/ws/upgrade.h"
#include "coop/io/recv_source.h"
#include "coop/io/buffer_ring.h"
#include "coop/io/uring.h"
#include "coop/thread.h"
#include <functional>
#include "coop/http/transport.h"
#include "coop/http/tls_transport.h"

using HttpConn = coop::http::Connection<coop::http::PlaintextTransport>;
using HttpClient = coop::http::ClientConnection<coop::http::PlaintextTransport>;

static constexpr size_t HTTP_EXTRA = HttpConn::ExtraBytes();

#include "test_helpers.h"
#include "send_test_helpers.h"
#include "tls_pair_helpers.h"
#include "coop/http/detail/server_impl.hpp"

namespace
{

int ConnectLoopbackKill(coop::io::Descriptor& client, uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return coop::io::ConnectKill(client, reinterpret_cast<sockaddr*>(&address),
                                  sizeof(address), std::chrono::seconds(1));
}

// Helper that creates an AF_UNIX socketpair.
//
struct SocketPair
{
    int fds[2];

    SocketPair()
    {
        int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(ret == 0);
        std::ignore = ret;
    }

    ~SocketPair()
    {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
};

// Send a string on a raw fd (not through io::). Used to feed request data to the Connection
// under test from the "client" side of the socketpair.
//
void SendString(coop::io::Descriptor& desc, const char* s)
{
    coop::io::SendAll(desc, s, strlen(s));
}

// Read all available data from a descriptor into a string.
//
std::string RecvAll(coop::io::Descriptor& desc, size_t maxBytes = 8192)
{
    std::string result;
    char buf[1024];

    while (result.size() < maxBytes)
    {
        int n = coop::io::Recv(desc, buf, sizeof(buf), 0,
                                std::chrono::milliseconds(100));
        if (n <= 0) break;
        result.append(buf, n);
    }

    return result;
}

} // end anonymous namespace

// -------------------------------------------------------------------------------------
// GET request line
// -------------------------------------------------------------------------------------

TEST(HttpTest, GetRequestLine)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->method, "GET");
        EXPECT_EQ(req->path, "/hello");
    });
}

TEST(HttpTest, PostRequestLine)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "POST /submit HTTP/1.1\r\nHost: localhost\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->method, "POST");
        EXPECT_EQ(req->path, "/submit");
    });
}

// -------------------------------------------------------------------------------------
// GET args (query string)
// -------------------------------------------------------------------------------------

TEST(HttpTest, GetArgs)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "GET /search?q=hello&lang=en HTTP/1.1\r\nHost: localhost\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->path, "/search");

        // First arg: q=hello
        //
        const char* name = conn->NextArgName();
        ASSERT_NE(name, nullptr);
        EXPECT_STREQ(name, "q");

        auto* chunk = conn->ReadArgValue();
        ASSERT_NE(chunk, nullptr);
        EXPECT_TRUE(chunk->complete);
        EXPECT_EQ(std::string_view(static_cast<const char*>(chunk->data), chunk->size), "hello");

        // Second arg: lang=en
        //
        name = conn->NextArgName();
        ASSERT_NE(name, nullptr);
        EXPECT_STREQ(name, "lang");

        chunk = conn->ReadArgValue();
        ASSERT_NE(chunk, nullptr);
        EXPECT_TRUE(chunk->complete);
        EXPECT_EQ(std::string_view(static_cast<const char*>(chunk->data), chunk->size), "en");

        // No more args
        //
        name = conn->NextArgName();
        EXPECT_EQ(name, nullptr);
    });
}

// -------------------------------------------------------------------------------------
// Headers
// -------------------------------------------------------------------------------------

TEST(HttpTest, Headers)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client,
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();

        // First header: Host
        //
        const char* name = conn->NextHeaderName();
        ASSERT_NE(name, nullptr);
        EXPECT_STREQ(name, "Host");

        auto* chunk = conn->ReadHeaderValue();
        ASSERT_NE(chunk, nullptr);
        EXPECT_TRUE(chunk->complete);
        EXPECT_EQ(std::string_view(static_cast<const char*>(chunk->data), chunk->size),
                  "localhost");

        // Second header: Content-Type
        //
        name = conn->NextHeaderName();
        ASSERT_NE(name, nullptr);
        EXPECT_STREQ(name, "Content-Type");

        chunk = conn->ReadHeaderValue();
        ASSERT_NE(chunk, nullptr);
        EXPECT_TRUE(chunk->complete);
        EXPECT_EQ(std::string_view(static_cast<const char*>(chunk->data), chunk->size),
                  "text/plain");

        // No more headers
        //
        name = conn->NextHeaderName();
        EXPECT_EQ(name, nullptr);
    });
}

// -------------------------------------------------------------------------------------
// POST with body (Content-Length)
// -------------------------------------------------------------------------------------

TEST(HttpTest, PostWithBody)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client,
            "POST /data HTTP/1.1\r\n"
            "Content-Length: 13\r\nHost: example.test\r\n"
            "\r\n"
            "Hello, World!");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->method, "POST");

        EXPECT_EQ(conn->ContentLength(), 13);

        // Read body
        //
        std::string body;
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
            if (chunk->complete) break;
        }
        EXPECT_EQ(body, "Hello, World!");
    });
}

// -------------------------------------------------------------------------------------
// Chunked body (Transfer-Encoding: chunked)
// -------------------------------------------------------------------------------------

TEST(HttpTest, ChunkedBody)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client,
            "POST /upload HTTP/1.1\r\n"
            "Transfer-Encoding: chunked\r\nHost: example.test\r\n"
            "\r\n"
            "5\r\n"
            "Hello\r\n"
            "7\r\n"
            ", World\r\n"
            "0\r\n"
            "\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);

        // Read body — chunked framing should be stripped
        //
        std::string body;
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
            if (chunk->complete) break;
        }
        // May need multiple ReadBody calls since chunks deliver separately
        //
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
            if (chunk->complete) break;
        }
        EXPECT_EQ(body, "Hello, World");
    });
}

// -------------------------------------------------------------------------------------
// Skip APIs — skip args, go straight to headers
// -------------------------------------------------------------------------------------

TEST(HttpTest, SkipArgsToHeaders)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client,
            "GET /path?a=1&b=2 HTTP/1.1\r\n"
            "X-Custom: test\r\nHost: example.test\r\n"
            "\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();

        // Skip args, go straight to headers
        //
        const char* name = conn->NextHeaderName();
        ASSERT_NE(name, nullptr);
        EXPECT_STREQ(name, "X-Custom");

        auto* chunk = conn->ReadHeaderValue();
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(std::string_view(static_cast<const char*>(chunk->data), chunk->size), "test");
    });
}

TEST(HttpTest, SkipHeadersToBody)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client,
            "POST /data HTTP/1.1\r\n"
            "Content-Length: 4\r\n"
            "X-Extra: ignored\r\nHost: example.test\r\n"
            "\r\n"
            "test");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();
        conn->SkipHeaders();

        EXPECT_EQ(conn->ContentLength(), 4);

        std::string body;
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
            if (chunk->complete) break;
        }
        EXPECT_EQ(body, "test");
    });
}

// -------------------------------------------------------------------------------------
// Send response
// -------------------------------------------------------------------------------------

TEST(HttpTest, SendResponse)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();
        conn->Send(200, "text/plain", "OK!\n");

        // Close server side so client recv gets EOF after data
        //
        server.Close();

        std::string resp = RecvAll(client);
        EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
        EXPECT_NE(resp.find("Content-Type: text/plain"), std::string::npos);
        EXPECT_NE(resp.find("Content-Length: 4"), std::string::npos);
        EXPECT_NE(resp.find("Connection: keep-alive"), std::string::npos);
        EXPECT_NE(resp.find("OK!\n"), std::string::npos);
    });
}

// -------------------------------------------------------------------------------------
// Chunked response
// -------------------------------------------------------------------------------------

TEST(HttpTest, ChunkedResponse)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();

        conn->BeginChunked(200, "text/plain");
        conn->SendChunk("Hello", 5);
        conn->SendChunk(", World", 7);
        conn->EndChunked();

        server.Close();

        std::string resp = RecvAll(client);
        EXPECT_NE(resp.find("Transfer-Encoding: chunked"), std::string::npos);
        EXPECT_NE(resp.find("5\r\nHello\r\n"), std::string::npos);
        EXPECT_NE(resp.find("7\r\n, World\r\n"), std::string::npos);
        EXPECT_NE(resp.find("0\r\n\r\n"), std::string::npos);
    });
}

TEST(HttpTest, Http10ResponsesCloseWithoutChunkFraming)
{
    coop::http::ServerHandle control;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::Context::Handle listener;
        coop::Spawn({.stackSize = 65536}, [&](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.handler = [](coop::http::ConnectionBase& conn, void*)
            {
                const bool fixed = conn.GetRequestLine()->path == "/fixed";
                conn.SkipHeaders();
                if (fixed)
                {
                    EXPECT_TRUE(conn.Send(200, "text/plain", "hello"));
                }
                else
                {
                    EXPECT_TRUE(conn.BeginChunked(200, "text/plain"));
                    EXPECT_TRUE(conn.SendChunk("hel", 3));
                    EXPECT_TRUE(conn.EndChunked("lo", 2));
                }
            };
            EXPECT_TRUE(coop::http::RunServer(serverCtx, config));
        }, &listener);

        const int port = control.Port();
        ASSERT_GT(port, 0);
        for (const char* path : {"/stream", "/fixed"})
        {
            for (bool keepAlive : {false, true})
            {
                coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
                ASSERT_GE(client.m_fd, 0);
                ASSERT_EQ(ConnectLoopbackKill(client, port), 0);
                const bool fixedKeepAlive = std::string_view(path) == "/fixed" && keepAlive;
                const std::string request = std::string("GET ") + path + " HTTP/1.0\r\n" +
                    (keepAlive ? "Connection: keep-alive\r\n" : "") + "\r\n" +
                    (fixedKeepAlive ? "GET /fixed HTTP/1.0\r\nConnection: close\r\n\r\n" : "");
                ASSERT_EQ(coop::io::SendKill(client, request.data(), request.size(), MSG_NOSIGNAL,
                          std::chrono::seconds(1)), static_cast<int>(request.size()));
                std::string wire;
                char bytes[1024];
                int result;
                while ((result = coop::io::RecvKill(client, bytes, sizeof(bytes), 0,
                                                std::chrono::seconds(1))) > 0)
                    wire.append(bytes, result);
                EXPECT_EQ(result, 0) << "response must end with orderly EOF";
                EXPECT_EQ(wire.find("HTTP/1.0 200 OK\r\n"), 0);
                EXPECT_NE(wire.find("Connection: close\r\n"), std::string::npos);
                if (fixedKeepAlive)
                {
                    EXPECT_NE(wire.find("Connection: keep-alive\r\n"), std::string::npos);
                    EXPECT_NE(wire.find("HTTP/1.0 200 OK\r\n", 1), std::string::npos);
                    continue;
                }
                EXPECT_EQ(wire.find("Transfer-Encoding:"), std::string::npos);
                EXPECT_EQ(wire.find("Trailer:"), std::string::npos);
                const auto end = wire.find("\r\n\r\n");
                ASSERT_NE(end, std::string::npos);
                EXPECT_EQ(wire.substr(end + 4), "hello");
                if (std::string_view(path) == "/fixed")
                    EXPECT_NE(wire.find("Content-Length: 5\r\n"), std::string::npos);
            }
        }
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        listener.Kill();
    });
}

TEST(HttpTest, Http11ChunkedResponseCompletesAndReuses)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        coop::io::Descriptor client(sp.fds[0]);
        coop::io::Descriptor server(sp.fds[1]);
        SendString(client, "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n"
                           "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n");
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            coop::http::PlaintextTransport(server), ctx, ctx->GetCooperator());
        ASSERT_NE(conn->GetRequestLine(), nullptr);
        ASSERT_TRUE(conn->BeginChunked(200, "text/plain"));
        ASSERT_TRUE(conn->EndChunked("hello", 5));
        ASSERT_TRUE(conn->SkipBody());
        ASSERT_TRUE(conn->Reset());
        ASSERT_NE(conn->GetRequestLine(), nullptr);
        EXPECT_EQ(conn->GetRequestLine()->path, "/next");
        const auto wire = RecvAll(client);
        EXPECT_EQ(wire.find("HTTP/1.1 200 OK\r\n"), 0);
        EXPECT_NE(wire.find("Transfer-Encoding: chunked\r\n"), std::string::npos);
        EXPECT_NE(wire.find("\r\n\r\n5\r\nhello\r\n0\r\n\r\n"), std::string::npos);
    });
}

// -------------------------------------------------------------------------------------
// Malformed request
// -------------------------------------------------------------------------------------

TEST(HttpTest, MalformedRequest)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        // Send garbage followed by \r\n — no space means no valid method/path split
        //
        SendString(client, "GARBAGE\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        auto* req = conn->GetRequestLine();
        EXPECT_EQ(req, nullptr);
    });
}

// -------------------------------------------------------------------------------------
// Timeout
// -------------------------------------------------------------------------------------

TEST(HttpTest, RecvTimeout)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        // Don't send anything — Connection should time out
        //
        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator(),
            HttpConn::DEFAULT_BUFFER_SIZE, HttpConn::DEFAULT_SEND_BUFFER_SIZE,
            std::chrono::milliseconds(50));

        auto* req = conn->GetRequestLine();
        EXPECT_EQ(req, nullptr);
    });
}

// ====================================================================================
// HTTP Client tests
// ====================================================================================

static constexpr size_t CLIENT_EXTRA = HttpClient::ExtraBytes();

// Helper: send a raw HTTP response on a descriptor.
//
void SendResponse(coop::io::Descriptor& desc, const char* s)
{
    coop::io::SendAll(desc, s, strlen(s));
}

// -------------------------------------------------------------------------------------
// Client: parse response status line
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, GetResponseLine)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendResponse(server, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        auto* resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 200);
        EXPECT_EQ(resp->reason, "OK");
    });
}

TEST(HttpClientTest, ResponseLine404)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendResponse(server, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        auto* resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 404);
        EXPECT_EQ(resp->reason, "Not Found");
    });
}

// -------------------------------------------------------------------------------------
// Client: parse response headers
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, ResponseHeaders)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendResponse(server,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "X-Custom: hello\r\n"
            "Content-Length: 0\r\n"
            "\r\n");

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        auto* resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 200);

        // Content-Type
        //
        const char* name = conn->NextHeaderName();
        ASSERT_NE(name, nullptr);
        EXPECT_STREQ(name, "Content-Type");
        auto* chunk = conn->ReadHeaderValue();
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(std::string_view(static_cast<const char*>(chunk->data), chunk->size),
                  "application/json");

        // X-Custom
        //
        name = conn->NextHeaderName();
        ASSERT_NE(name, nullptr);
        EXPECT_STREQ(name, "X-Custom");
        chunk = conn->ReadHeaderValue();
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(std::string_view(static_cast<const char*>(chunk->data), chunk->size),
                  "hello");

        // Content-Length (special header — captured internally)
        //
        name = conn->NextHeaderName();
        ASSERT_NE(name, nullptr);
        EXPECT_STREQ(name, "Content-Length");
        conn->SkipHeaderValue();

        // End of headers
        //
        EXPECT_EQ(conn->NextHeaderName(), nullptr);
        EXPECT_EQ(conn->ContentLength(), 0);
    });
}

// -------------------------------------------------------------------------------------
// Client: read body with Content-Length
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, ResponseBody)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendResponse(server,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "Hello, World!");

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        auto* resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 200);

        conn->SkipHeaders();
        EXPECT_EQ(conn->ContentLength(), 13);

        std::string body;
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
            if (chunk->complete) break;
        }
        EXPECT_EQ(body, "Hello, World!");
    });
}

// -------------------------------------------------------------------------------------
// Client: chunked response body
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, ChunkedResponseBody)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendResponse(server,
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "5\r\n"
            "Hello\r\n"
            "7\r\n"
            ", World\r\n"
            "0\r\n"
            "\r\n");

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        conn->GetResponseLine();
        conn->SkipHeaders();

        std::string body;
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
        }
        EXPECT_EQ(body, "Hello, World");
    });
}

// -------------------------------------------------------------------------------------
// Client: send GET request
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, SendGetRequest)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "example.com");

        bool ok = conn->Get("/plaintext");
        EXPECT_TRUE(ok);

        // Read what the server side received
        //
        std::string req = RecvAll(server);
        EXPECT_NE(req.find("GET /plaintext HTTP/1.1\r\n"), std::string::npos);
        EXPECT_NE(req.find("Host: example.com\r\n"), std::string::npos);
    });
}

// -------------------------------------------------------------------------------------
// Client: send POST request with body
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, SendPostRequest)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "example.com");

        const char* body = R"({"key":"value"})";
        bool ok = conn->Post("/api/data", "application/json", body, strlen(body));
        EXPECT_TRUE(ok);

        std::string req = RecvAll(server);
        EXPECT_NE(req.find("POST /api/data HTTP/1.1\r\n"), std::string::npos);
        EXPECT_NE(req.find("Host: example.com\r\n"), std::string::npos);
        EXPECT_NE(req.find("Content-Type: application/json\r\n"), std::string::npos);
        EXPECT_NE(req.find("Content-Length: 15\r\n"), std::string::npos);
        EXPECT_NE(req.find(body), std::string::npos);
    });
}

// -------------------------------------------------------------------------------------
// Client: roundtrip — send request, parse response
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, Roundtrip)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        // Client sends GET
        //
        conn->Get("/hello");

        // Server side: read request, send response
        //
        std::string req = RecvAll(server);
        EXPECT_NE(req.find("GET /hello"), std::string::npos);

        SendResponse(server,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "world");

        // Client parses response
        //
        auto* resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 200);

        conn->SkipHeaders();

        std::string body;
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
        }
        EXPECT_EQ(body, "world");
    });
}

// -------------------------------------------------------------------------------------
// Client: keep-alive — multiple requests on one connection
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, KeepAlive)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        for (int i = 0; i < 3; i++)
        {
            conn->Get("/ping");

            // Drain request on server side
            //
            RecvAll(server);

            // Server responds
            //
            SendResponse(server,
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 4\r\n"
                "Connection: keep-alive\r\n"
                "\r\n"
                "pong");

            auto* resp = conn->GetResponseLine();
            ASSERT_NE(resp, nullptr);
            EXPECT_EQ(resp->status, 200);

            conn->SkipHeaders();

            std::string body;
            while (auto* chunk = conn->ReadBody())
            {
                body.append(static_cast<const char*>(chunk->data), chunk->size);
            }
            EXPECT_EQ(body, "pong");
            EXPECT_TRUE(conn->KeepAlive());

            conn->SkipBody();
            conn->Reset();
        }
    });
}

// -------------------------------------------------------------------------------------
// Client: Connection: close detection
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, ConnectionClose)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendResponse(server,
            "HTTP/1.1 200 OK\r\n"
            "Connection: close\r\n"
            "Content-Length: 2\r\n"
            "\r\n"
            "OK");

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        conn->GetResponseLine();
        conn->SkipHeaders();
        conn->SkipBody();

        EXPECT_FALSE(conn->KeepAlive());
    });
}

// -------------------------------------------------------------------------------------
// Client: connection closed before response — GetResponseLine returns null
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, ConnectionClosedReturnsNull)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        server.Close();

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        EXPECT_EQ(conn->GetResponseLine(), nullptr);
    });
}

// -------------------------------------------------------------------------------------
// Client: malformed response — GetResponseLine returns null
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, MalformedResponseReturnsNull)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendResponse(server, "GARBAGE\r\n\r\n");

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        EXPECT_EQ(conn->GetResponseLine(), nullptr);
    });
}

// -------------------------------------------------------------------------------------
// Request target: raw path + query for forwarding
// -------------------------------------------------------------------------------------

TEST(HttpTest, RequestTargetWithQuery)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client,
            "GET /bucket/key?versionId=abc123&partNumber=2 HTTP/1.1\r\n"
            "Host: localhost\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->path, "/bucket/key");
        EXPECT_EQ(req->query, "versionId=abc123&partNumber=2");
        EXPECT_EQ(req->target, "/bucket/key?versionId=abc123&partNumber=2");

        // Args parsing is unaffected by target capture
        //
        const char* name = conn->NextArgName();
        ASSERT_NE(name, nullptr);
        EXPECT_STREQ(name, "versionId");
    });
}

TEST(HttpTest, RequestTargetNoQuery)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "GET /bucket/key HTTP/1.1\r\nHost: localhost\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_TRUE(req->query.empty());
        EXPECT_EQ(req->target, "/bucket/key");
    });
}

// -------------------------------------------------------------------------------------
// Component response API: BeginResponse / AppendHeader / EndHeaders
// -------------------------------------------------------------------------------------

TEST(HttpTest, ComponentResponseCustomHeaders)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "GET /obj HTTP/1.1\r\nHost: localhost\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();

        EXPECT_TRUE(conn->BeginResponse(206));
        EXPECT_TRUE(conn->AppendHeader("Content-Type", "application/octet-stream"));
        EXPECT_TRUE(conn->AppendHeader("Content-Range", "bytes 2-6/10"));
        EXPECT_TRUE(conn->AppendHeader("ETag", "\"abc123\""));
        EXPECT_TRUE(conn->AppendHeader("Content-Length", size_t(5)));
        EXPECT_TRUE(conn->EndHeaders());
        EXPECT_TRUE(conn->SendRawBytes("llo w", 5));

        server.Close();

        std::string resp = RecvAll(client);
        EXPECT_NE(resp.find("HTTP/1.1 206 Partial Content\r\n"), std::string::npos);
        EXPECT_NE(resp.find("Content-Range: bytes 2-6/10\r\n"), std::string::npos);
        EXPECT_NE(resp.find("ETag: \"abc123\"\r\n"), std::string::npos);
        EXPECT_NE(resp.find("Content-Length: 5\r\n"), std::string::npos);
        EXPECT_NE(resp.find("Connection: keep-alive\r\n\r\nllo w"), std::string::npos);
    });
}

TEST(HttpTest, ComponentResponseRuntimeStatus)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();

        // Status outside the pre-compiled table, with an upstream reason phrase (a
        // string_view, not null-terminated — the proxy passthrough case)
        //
        std::string upstream = "Slow Down Please";
        EXPECT_TRUE(conn->BeginResponse(599, std::string_view(upstream)));
        EXPECT_TRUE(conn->AppendHeader("Content-Length", size_t(0)));
        EXPECT_TRUE(conn->EndHeaders());

        server.Close();

        std::string resp = RecvAll(client);
        EXPECT_NE(resp.find("HTTP/1.1 599 Slow Down Please\r\n"), std::string::npos);
    });
}

TEST(HttpTest, ComponentResponseDefaultReason)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();

        EXPECT_TRUE(conn->BeginResponse(418));
        EXPECT_TRUE(conn->AppendHeader("Content-Length", size_t(0)));
        EXPECT_TRUE(conn->EndHeaders());

        server.Close();

        std::string resp = RecvAll(client);
        EXPECT_NE(resp.find("HTTP/1.1 418 Client Error\r\n"), std::string::npos);
    });
}

TEST(HttpTest, ForceCloseSetsConnectionClose)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();

        conn->ForceClose();
        EXPECT_FALSE(conn->KeepAlive());

        EXPECT_TRUE(conn->BeginResponse(200));
        EXPECT_TRUE(conn->EndHeaders());
        EXPECT_TRUE(conn->SendRawBytes("unframed body", 13));

        server.Close();

        std::string resp = RecvAll(client);
        EXPECT_NE(resp.find("Connection: close\r\n\r\nunframed body"), std::string::npos);
    });
}

// -------------------------------------------------------------------------------------
// Client: component request API — BeginRequest / AppendHeader / SendBody
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, ComponentRequestCustomHeaders)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "bucket.s3.amazonaws.com");

        EXPECT_TRUE(conn->BeginRequest("PUT", "/key?partNumber=1&uploadId=xyz"));
        EXPECT_TRUE(conn->AppendHeader("Authorization",
            "AWS4-HMAC-SHA256 Credential=AKID/20260804/us-east-1/s3/aws4_request"));
        EXPECT_TRUE(conn->AppendHeader("x-amz-content-sha256", "UNSIGNED-PAYLOAD"));
        EXPECT_TRUE(conn->AppendHeader("Content-Length", size_t(8)));
        EXPECT_TRUE(conn->EndHeaders());

        // Stream the body in two pieces
        //
        EXPECT_TRUE(conn->SendBody("part", 4));
        EXPECT_TRUE(conn->SendBody("data", 4));

        std::string req = RecvAll(server);
        EXPECT_NE(req.find("PUT /key?partNumber=1&uploadId=xyz HTTP/1.1\r\n"),
                  std::string::npos);
        EXPECT_NE(req.find("Host: bucket.s3.amazonaws.com\r\n"), std::string::npos);
        EXPECT_NE(req.find("Authorization: AWS4-HMAC-SHA256"), std::string::npos);
        EXPECT_NE(req.find("x-amz-content-sha256: UNSIGNED-PAYLOAD\r\n"),
                  std::string::npos);
        EXPECT_NE(req.find("Content-Length: 8\r\n"), std::string::npos);
        EXPECT_NE(req.find("\r\n\r\npartdata"), std::string::npos);
    });
}

// -------------------------------------------------------------------------------------
// Client: HEAD and 304 responses are framing-only — no body bytes on the wire
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, HeadResponseHasNoBody)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        EXPECT_TRUE(conn->Head("/object"));

        std::string req = RecvAll(server);
        EXPECT_NE(req.find("HEAD /object HTTP/1.1\r\n"), std::string::npos);

        // HEAD response advertises the entity's length but carries no body
        //
        SendResponse(server,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 1234\r\n"
            "ETag: \"abc\"\r\n"
            "\r\n");

        auto* resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 200);
        conn->SkipHeaders();

        // Content-Length is still reported (a proxy forwards it), but ReadBody ends
        // immediately instead of waiting for 1234 bytes that will never arrive
        //
        EXPECT_EQ(conn->ContentLength(), 1234);
        EXPECT_EQ(conn->ReadBody(), nullptr);

        // The connection remains usable: a second request/response parses cleanly
        //
        conn->Reset();
        EXPECT_TRUE(conn->Get("/next"));
        RecvAll(server);
        SendResponse(server,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "\r\n"
            "ok");

        resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        conn->SkipHeaders();

        std::string body;
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
        }
        EXPECT_EQ(body, "ok");
    });
}

TEST(HttpClientTest, NotModifiedResponseHasNoBody)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        EXPECT_TRUE(conn->Get("/cached"));
        RecvAll(server);

        SendResponse(server,
            "HTTP/1.1 304 Not Modified\r\n"
            "Content-Length: 512\r\n"
            "ETag: \"abc\"\r\n"
            "\r\n");

        auto* resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 304);
        conn->SkipHeaders();
        EXPECT_EQ(conn->ReadBody(), nullptr);
    });
}

// -------------------------------------------------------------------------------------
// Proxy passthrough: client -> proxy (server conn + client conn) -> origin
// -------------------------------------------------------------------------------------

TEST(HttpProxyTest, PassThrough)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto* uring = coop::GetUring();

        // Socketpair A: test client <-> proxy's server-side connection
        // Socketpair B: proxy's upstream client connection <-> origin
        //
        SocketPair a;
        SocketPair b;
        coop::io::Descriptor testClient(a.fds[0], uring);
        coop::io::Descriptor proxyDownstream(a.fds[1], uring);
        coop::io::Descriptor proxyUpstream(b.fds[0], uring);
        coop::io::Descriptor origin(b.fds[1], uring);

        // Test client sends a ranged GET with conditional headers
        //
        SendString(testClient,
            "GET /bucket/key?versionId=v7 HTTP/1.1\r\n"
            "Host: proxy.local\r\n"
            "Range: bytes=2-6\r\n"
            "If-Match: \"etag1\"\r\n"
            "\r\n");

        // Proxy: parse the downstream request
        //
        coop::http::PlaintextTransport downTransport(proxyDownstream);
        auto down = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            downTransport, ctx, ctx->GetCooperator());

        auto* req = down->GetRequestLine();
        ASSERT_NE(req, nullptr);
        std::string method(req->method);
        std::string target(req->target);

        // Forward selected headers upstream. Header values are consumed as chunks;
        // for this test they always fit in one chunk.
        //
        coop::http::PlaintextTransport upTransport(proxyUpstream);
        auto up = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            upTransport, "bucket.s3.amazonaws.com");

        ASSERT_TRUE(up->BeginRequest(method.c_str(), target.c_str()));
        while (auto* name = down->NextHeaderName())
        {
            if (strcasecmp(name, "Range") == 0 || strcasecmp(name, "If-Match") == 0)
            {
                std::string headerName(name);
                auto* value = down->ReadHeaderValue();
                ASSERT_NE(value, nullptr);
                ASSERT_TRUE(value->complete);
                ASSERT_TRUE(up->AppendHeader(headerName.c_str(),
                    std::string_view(static_cast<const char*>(value->data),
                                     value->size)));
            }
            else
            {
                down->SkipHeaderValue();
            }
        }
        ASSERT_TRUE(up->AppendHeader("Authorization", "AWS4-HMAC-SHA256 test"));
        ASSERT_TRUE(up->EndHeaders());

        // Origin: verify the forwarded request, respond 206 with entity headers
        //
        std::string upstreamReq = RecvAll(origin);
        EXPECT_NE(upstreamReq.find("GET /bucket/key?versionId=v7 HTTP/1.1\r\n"),
                  std::string::npos);
        EXPECT_NE(upstreamReq.find("Host: bucket.s3.amazonaws.com\r\n"),
                  std::string::npos);
        EXPECT_NE(upstreamReq.find("Range: bytes=2-6\r\n"), std::string::npos);
        EXPECT_NE(upstreamReq.find("If-Match: \"etag1\"\r\n"), std::string::npos);
        EXPECT_NE(upstreamReq.find("Authorization: AWS4-HMAC-SHA256 test\r\n"),
                  std::string::npos);

        SendResponse(origin,
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Range: bytes 2-6/10\r\n"
            "Content-Length: 5\r\n"
            "ETag: \"etag1\"\r\n"
            "x-amz-request-id: REQ123\r\n"
            "\r\n"
            "llo w");

        // Proxy: forward status line, selected headers, and body downstream
        //
        auto* resp = up->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 206);
        ASSERT_TRUE(down->BeginResponse(resp->status, resp->reason));

        while (auto* name = up->NextHeaderName())
        {
            std::string headerName(name);
            auto* value = up->ReadHeaderValue();
            ASSERT_NE(value, nullptr);
            ASSERT_TRUE(value->complete);
            ASSERT_TRUE(down->AppendHeader(headerName.c_str(),
                std::string_view(static_cast<const char*>(value->data), value->size)));
        }
        ASSERT_TRUE(down->EndHeaders());

        while (auto* chunk = up->ReadBody())
        {
            ASSERT_TRUE(down->SendRawBytes(chunk->data, chunk->size));
        }

        proxyDownstream.Close();

        // Test client: everything passed through
        //
        std::string finalResp = RecvAll(testClient);
        EXPECT_NE(finalResp.find("HTTP/1.1 206 Partial Content\r\n"), std::string::npos);
        EXPECT_NE(finalResp.find("Content-Range: bytes 2-6/10\r\n"), std::string::npos);
        EXPECT_NE(finalResp.find("ETag: \"etag1\"\r\n"), std::string::npos);
        EXPECT_NE(finalResp.find("x-amz-request-id: REQ123\r\n"), std::string::npos);
        EXPECT_NE(finalResp.find("Content-Length: 5\r\n"), std::string::npos);
        EXPECT_NE(finalResp.find("\r\n\r\nllo w"), std::string::npos);
    });
}

// -------------------------------------------------------------------------------------
// Receive-to-disk: ReadBodyToFile
// -------------------------------------------------------------------------------------

namespace
{

// A body large enough that header parsing leaves most of it in the socket: the leftover
// drain covers the recv-buffer slice and the splice path moves the rest.
//
std::string PatternBody(size_t size)
{
    std::string body;
    body.reserve(size);
    while (body.size() < size)
    {
        body.append("0123456789abcdef");
    }
    body.resize(size);
    return body;
}

int MakeTmpFile()
{
    char tmpPath[] = "/tmp/coop_http_body_XXXXXX";
    int fd = mkstemp(tmpPath);
    if (fd >= 0) unlink(tmpPath);
    return fd;
}

std::string ReadFileAt(int fd, off_t offset, size_t size)
{
    std::string content(size, '\0');
    ssize_t r = ::pread(fd, content.data(), size, offset);
    content.resize(r > 0 ? r : 0);
    return content;
}

} // end anonymous namespace

TEST(HttpTest, ReadBodyToFileSplicesAndPreservesPipelining)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();

        // Splice requires a non-blocking socket (production accepted sockets already are)
        //
        fcntl(sp.fds[1], F_SETFL, fcntl(sp.fds[1], F_GETFL) | O_NONBLOCK);

        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        // 8KB body: far larger than the 2KB recv buffer. A second pipelined request rides
        // directly behind the body — the splice length is framed, so it must survive.
        //
        std::string body = PatternBody(8192);
        std::string request =
            "PUT /bucket/key HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 8192\r\n"
            "\r\n" + body +
            "GET /after HTTP/1.1\r\nHost: localhost\r\n\r\n";
        coop::io::SendAll(client, request.data(), request.size());

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());

        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->method, "PUT");
        conn->SkipHeaders();

        int fileFd = MakeTmpFile();
        ASSERT_GE(fileFd, 0);

        int64_t written = conn->ReadBodyToFile(fileFd, 0);
        ASSERT_EQ(written, 8192);
        EXPECT_EQ(ReadFileAt(fileFd, 0, 8192), body);

        conn->Send(200, "text/plain", "");

        // The pipelined GET was not consumed by the splice
        //
        conn->Reset();
        req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->method, "GET");
        EXPECT_EQ(req->path, "/after");

        ::close(fileFd);
    });
}

TEST(HttpTest, KilledChildBodyToFileStopsItsOwnSpliceWait)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);
        coop::io::Descriptor client(fds[0]);
        coop::io::Descriptor server(fds[1]);
        const std::string headers =
            "PUT /body HTTP/1.1\r\nContent-Length: 4194304\r\nHost: example.test\r\n\r\n";
        ASSERT_EQ(coop::io::SendAll(client, headers.data(), headers.size()), headers.size());
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            coop::http::PlaintextTransport(server), ctx, ctx->GetCooperator());
        ASSERT_NE(conn->GetRequestLine(), nullptr);
        conn->SkipHeaders();
        coop::io::Descriptor file(MakeTmpFile());
        ASSERT_GE(file.m_fd, 0);
        coop::CountdownLatch joined(1);
        coop::Context::Handle reader;
        ASSERT_TRUE(coop::Spawn([&](coop::Context* child)
        {
            EXPECT_EQ(conn->ReadBodyToFile(file.m_fd, 0), -ECANCELED);
            EXPECT_TRUE(child->IsKilled());
            joined.Arrive();
        }, &reader));
        reader.Kill();
        // Wake the old non-cancelable splice as well, so its wrong result is a
        // deterministic failure rather than a test that needs a rescue timer.
        //
        std::string tail(8192, 'x');
        EXPECT_EQ(coop::io::SendAll(client, tail.data(), tail.size()), tail.size());
        EXPECT_EQ(shutdown(client.m_fd, SHUT_WR), 0);
        joined.Wait(ctx);
        EXPECT_FALSE(ctx->IsKilled());
        struct stat status{};
        ASSERT_EQ(fstat(file.m_fd, &status), 0);
        EXPECT_EQ(status.st_size, 0);
    });
}

TEST(HttpTest, ChildBodyToFileYieldsItsOwnContext)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);
        coop::io::Descriptor client(fds[0]);
        coop::io::Descriptor server(fds[1]);
        std::string body = PatternBody(4 * 1024 * 1024);
        std::string headers = "PUT /body HTTP/1.1\r\nHost: example.test\r\nContent-Length: "
            + std::to_string(body.size()) + "\r\n\r\n";
        ASSERT_EQ(coop::io::SendAll(client, headers.data(), headers.size()), headers.size());
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            coop::http::PlaintextTransport(server), ctx, ctx->GetCooperator());
        ASSERT_NE(conn->GetRequestLine(), nullptr);
        conn->SkipHeaders();
        coop::io::Descriptor file(MakeTmpFile());
        ASSERT_GE(file.m_fd, 0);
        coop::CountdownLatch joined(2);
        ASSERT_TRUE(coop::Spawn([&](coop::Context*)
        {
            EXPECT_EQ(coop::io::SendAll(client, body.data(), body.size()), body.size());
            joined.Arrive();
        }));
        ASSERT_TRUE(coop::Spawn([&](coop::Context* child)
        {
            EXPECT_EQ(conn->ReadBodyToFile(file.m_fd, 0), body.size());
            EXPECT_EQ(coop::Self(), child);
            EXPECT_FALSE(child->IsKilled());
            joined.Arrive();
        }));
        joined.Wait(ctx);
        EXPECT_FALSE(ctx->IsKilled());
        EXPECT_EQ(ReadFileAt(file.m_fd, 0, body.size()), body);
    });
}

namespace
{
struct CancelingBodyTransport : test::ShortWriteTransport
{
    std::string* input;
    bool* cancel;
    int Recv(void* data, size_t size, int, coop::time::Interval)
    {
        if (*cancel) coop::Self()->KillSelf(coop::KillCause::Kill);
        size_t n = std::min(size, input->size());
        memcpy(data, input->data(), n);
        input->erase(0, n);
        return static_cast<int>(n);
    }
};
}

TEST(HttpTest, ChildBodyToFileObservesItsOwnCancellation)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        test::SendScript script;
        coop::io::Descriptor descriptor(coop::io::borrowed, -1);
        std::string input = "PUT /body HTTP/1.1\r\nContent-Length: 4194304\r\nHost: example.test\r\n\r\n";
        bool cancel = false;
        CancelingBodyTransport transport{{&script, &descriptor}, &input, &cancel};
        using Connection = coop::http::Connection<CancelingBodyTransport>;
        auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
            transport, ctx, ctx->GetCooperator());
        ASSERT_NE(conn->GetRequestLine(), nullptr);
        conn->SkipHeaders();
        input.assign(4 * 1024 * 1024, 'x');
        cancel = true;
        coop::io::Descriptor file(MakeTmpFile());
        ASSERT_GE(file.m_fd, 0);
        coop::CountdownLatch joined(1);
        ASSERT_TRUE(coop::Spawn([&](coop::Context* child)
        {
            EXPECT_EQ(conn->ReadBodyToFile(file.m_fd, 0), -ECANCELED);
            EXPECT_TRUE(child->IsKilled());
            joined.Arrive();
        }));
        joined.Wait(ctx);
        EXPECT_FALSE(ctx->IsKilled());
        struct stat status{};
        ASSERT_EQ(fstat(file.m_fd, &status), 0);
        EXPECT_EQ(status.st_size, 0);
    });
}

TEST(HttpTest, CompletedBodyReadsRemainNoOpsAfterCancellation)
{
    for (bool consumed : {false, true})
        test::RunInCooperator([&](coop::Context* ctx)
        {
            test::SendScript script;
            coop::io::Descriptor descriptor(coop::io::borrowed, -1);
            std::string input = consumed
                ? "POST /body HTTP/1.0\r\nContent-Length: 3\r\n\r\nabc"
                : "GET /body HTTP/1.0\r\n\r\n";
            bool cancel = false;
            CancelingBodyTransport transport{{&script, &descriptor}, &input, &cancel};
            using Connection = coop::http::Connection<CancelingBodyTransport>;
            auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
                transport, ctx, ctx->GetCooperator());
            ASSERT_NE(conn->GetRequestLine(), nullptr);
            conn->SkipHeaders();
            if (consumed) ASSERT_TRUE(conn->SkipBody());
            ASSERT_TRUE(conn->BeginChunked(200, "text/plain"));
            ASSERT_TRUE(conn->EndChunked("complete", 8));
            ctx->KillSelf(coop::KillCause::Kill);
            EXPECT_TRUE(conn->SkipBody());
            EXPECT_EQ(conn->ReadBodyToFile(-1, 0), 0);
            EXPECT_EQ(conn->Error(), 0);
            EXPECT_TRUE(conn->CloseDelimitedResponseComplete());
        });
}

TEST(HttpTest, PlaintextSendFastpathOutsideCooperator)
{
    coop::io::Uring ring;
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);
    coop::io::Descriptor descriptor(coop::io::borrowed, fds[0], &ring);
    coop::http::PlaintextTransport transport(descriptor);
    EXPECT_EQ(transport.Send("x", 1, {}), 1);
    char byte = 0;
    EXPECT_EQ(recv(fds[1], &byte, 1, 0), 1);
    EXPECT_EQ(byte, 'x');
    std::string fill(4096, 'x');
    while (send(fds[0], fill.data(), fill.size(), MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {}
    EXPECT_EQ(errno, EAGAIN);
    EXPECT_EQ(transport.Send("x", 1, std::chrono::seconds(1)), -EAGAIN);
    close(fds[0]);
    close(fds[1]);
}

TEST(HttpTest, ReadBodyToFileChunked)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client,
            "PUT /obj HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "5\r\nhello\r\n"
            "7\r\n, world\r\n"
            "0\r\n\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());
        conn->GetRequestLine();
        conn->SkipHeaders();

        int fileFd = MakeTmpFile();
        ASSERT_GE(fileFd, 0);

        int64_t written = conn->ReadBodyToFile(fileFd, 0);
        ASSERT_EQ(written, 12);
        EXPECT_EQ(ReadFileAt(fileFd, 0, 12), "hello, world");

        ::close(fileFd);
    });
}

TEST(HttpClientTest, ReadBodyToFileCacheFill)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();

        fcntl(sp.fds[0], F_SETFL, fcntl(sp.fds[0], F_GETFL) | O_NONBLOCK);

        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "origin.example");

        EXPECT_TRUE(conn->Get("/object"));
        RecvAll(server);

        // 16KB object: recv buffer is 4KB, so most of the body splices. A second
        // keep-alive response follows later on the same connection.
        //
        std::string body = PatternBody(16384);
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 16384\r\n"
            "ETag: \"cache-me\"\r\n"
            "\r\n" + body;
        coop::io::SendAll(server, response.data(), response.size());

        auto* resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 200);
        conn->SkipHeaders();

        int fileFd = MakeTmpFile();
        ASSERT_GE(fileFd, 0);

        int64_t written = conn->ReadBodyToFile(fileFd, 0);
        ASSERT_EQ(written, 16384);
        EXPECT_EQ(ReadFileAt(fileFd, 0, 16384), body);

        // Connection is positioned for keep-alive reuse
        //
        conn->Reset();
        EXPECT_TRUE(conn->Get("/next"));
        RecvAll(server);
        SendResponse(server,
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

        resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        conn->SkipHeaders();
        std::string small;
        while (auto* chunk = conn->ReadBody())
        {
            small.append(static_cast<const char*>(chunk->data), chunk->size);
        }
        EXPECT_EQ(small, "ok");

        ::close(fileFd);
    });
}

TEST(HttpClientTest, ReadBodyToFileHeadIsEmpty)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "localhost");

        EXPECT_TRUE(conn->Head("/object"));
        RecvAll(server);
        SendResponse(server,
            "HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n");

        ASSERT_NE(conn->GetResponseLine(), nullptr);
        conn->SkipHeaders();

        int fileFd = MakeTmpFile();
        ASSERT_GE(fileFd, 0);

        EXPECT_EQ(conn->ReadBodyToFile(fileFd, 0), 0);

        struct stat st;
        ASSERT_EQ(fstat(fileFd, &st), 0);
        EXPECT_EQ(st.st_size, 0);

        ::close(fileFd);
    });
}

// -------------------------------------------------------------------------------------
// Send-from-disk: client SendBodyFromFile (PUT of a cached object)
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, SendBodyFromFile)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();

        // Sendfile requires a non-blocking socket
        //
        fcntl(sp.fds[0], F_SETFL, fcntl(sp.fds[0], F_GETFL) | O_NONBLOCK);

        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        int fileFd = MakeTmpFile();
        ASSERT_GE(fileFd, 0);
        std::string content = PatternBody(4000);
        ASSERT_EQ(::pwrite(fileFd, content.data(), content.size(), 0),
                  (ssize_t)content.size());

        coop::http::PlaintextTransport transport(client);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "bucket.s3.amazonaws.com");

        EXPECT_TRUE(conn->BeginRequest("PUT", "/key"));
        EXPECT_TRUE(conn->AppendHeader("Content-Length", content.size()));
        EXPECT_TRUE(conn->EndHeaders());
        EXPECT_TRUE(conn->SendBodyFromFile(fileFd, 0, content.size()));

        std::string req = RecvAll(server, 8192);
        EXPECT_NE(req.find("PUT /key HTTP/1.1\r\n"), std::string::npos);
        EXPECT_NE(req.find("Content-Length: 4000\r\n"), std::string::npos);

        // Everything after the header block is the file body, byte for byte
        //
        size_t bodyStart = req.find("\r\n\r\n");
        ASSERT_NE(bodyStart, std::string::npos);
        std::string bodyOnWire = req.substr(bodyStart + 4);
        while (bodyOnWire.size() < content.size())
        {
            std::string more = RecvAll(server, content.size() - bodyOnWire.size());
            if (more.empty()) break;
            bodyOnWire += more;
        }
        EXPECT_EQ(bodyOnWire, content);

        ::close(fileFd);
    });
}

// -------------------------------------------------------------------------------------
// RunServer with declared configuration: multishot accept path, roundtrip, kill exit
// -------------------------------------------------------------------------------------

namespace
{

void OkHandler(coop::http::ConnectionBase& conn, void*)
{
    conn.Send(200, "text/plain", "OK");
}

void ExerciseServerBindAddress(bool loopbackOnly, bool tls)
{
    test::RunInCooperator(
        [&](coop::Context* ctx)
        {
            coop::http::ServerHandle control;
            coop::io::ssl::Context sslContext{coop::io::ssl::Mode::Server};
            coop::http::ServerConfiguration config;
            config.port = 0;
            if (loopbackOnly)
                config.bindAddress = "127.0.0.1";
            config.control = &control;
            config.handler = &OkHandler;
            config.timeout = std::chrono::milliseconds(500);
            coop::Context::Handle listener;
            bool returned = false;
            EXPECT_TRUE(coop::Spawn(
                {.stackSize = 65536},
                [&](coop::Context* server)
                {
                    EXPECT_TRUE(tls ? coop::http::RunTlsServer(server, config, sslContext)
                                    : coop::http::RunServer(server, config));
                    returned = true;
                },
                &listener));
            const auto port = control.Port();
            EXPECT_NE(port, 0);
            if (port != 0)
            {
                // Binding precedes TLS handshakes. TCP acceptance alone observes the
                // interface restriction without needing certificates or HTTP traffic.
                //
                for (uint32_t host : {INADDR_LOOPBACK, INADDR_LOOPBACK + 1U})
                {
                    SCOPED_TRACE(host);
                    coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
                    EXPECT_GE(client.m_fd, 0);
                    if (client.m_fd < 0)
                        continue;
                    sockaddr_in address{};
                    address.sin_family = AF_INET;
                    address.sin_addr.s_addr = htonl(host);
                    address.sin_port = htons(port);
                    int result =
                        coop::io::ConnectKill(client, reinterpret_cast<sockaddr*>(&address),
                                              sizeof(address), std::chrono::seconds(1));
                    EXPECT_EQ(result, loopbackOnly && host != INADDR_LOOPBACK ? -ECONNREFUSED : 0);
                }
            }
            EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
            listener.Kill();
            ctx->JoinChildren();
            EXPECT_TRUE(returned);
        },
        {.stackSize = 65536});
}

} // end anonymous namespace

TEST(HttpTest, BindAddressRestrictsPlaintextAndTlsListeners)
{
    for (bool tls : {false, true})
    {
        SCOPED_TRACE(tls);
        ExerciseServerBindAddress(true, tls);
    }
}

TEST(HttpTest, BindAddressDefaultPreservesWildcardListeners)
{
    for (bool tls : {false, true})
    {
        SCOPED_TRACE(tls);
        ExerciseServerBindAddress(false, tls);
    }
}

TEST(HttpTest, BindAddressRejectsInvalidInputWithoutListener)
{
    for (bool tls : {false, true})
    {
        SCOPED_TRACE(tls);
        const char* invalidAddresses[] = {nullptr, "", "localhost", "127.0.0.999", "::1"};
        for (const char* address : invalidAddresses)
        {
            SCOPED_TRACE(address ? address : "(null)");
            test::RunInCooperator(
                [&](coop::Context* ctx)
                {
                    coop::http::ServerHandle control;
                    coop::io::ssl::Context sslContext{coop::io::ssl::Mode::Server};
                    coop::http::ServerConfiguration config;
                    config.port = 0;
                    config.bindAddress = address;
                    config.control = &control;
                    config.handler = &OkHandler;
                    coop::Context::Handle listener;
                    bool returned = false;
                    bool result = true;
                    EXPECT_TRUE(coop::Spawn(
                        {.stackSize = 65536},
                        [&](coop::Context* server)
                        {
                            result = tls ? coop::http::RunTlsServer(server, config, sslContext)
                                         : coop::http::RunServer(server, config);
                            returned = true;
                        },
                        &listener));
                    EXPECT_TRUE(returned);
                    EXPECT_FALSE(result);
                    EXPECT_FALSE(control.HasListener());
                    EXPECT_EQ(control.Port(), 0);
                    listener.Kill();
                    ctx->JoinChildren();
                },
                {.stackSize = 65536});
        }
    }
}

TEST(HttpTest, RunServerMultishotAcceptRoundtrip)
{
    coop::http::ServerHandle control;
    bool serverReturned = false;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::Context::Handle serverHandle;
        ctx->GetCooperator()->Spawn(
            {.priority = 0, .stackSize = 65536},
            [&](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.multishotAccept = true;
            config.maxPendingAccepts = 8;
            config.name = "TestMultishotServer";
            config.handler = &OkHandler;
            coop::http::RunServer(serverCtx, config);
            serverReturned = true;
        }, &serverHandle);

        const auto port = control.Port();
        ASSERT_NE(port, 0);

        for (int round = 0; round < 2; round++)
        {
            int cfd = socket(AF_INET, SOCK_STREAM, 0);
            ASSERT_GE(cfd, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            ASSERT_EQ(connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0)
                << "round " << round << ": " << strerror(errno);

            coop::io::Descriptor client(cfd, coop::GetUring());
            const char* req = "GET /ok HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
            ASSERT_GT(coop::io::SendAll(client, req, strlen(req)), 0);

            std::string resp = RecvAll(client, 4096);
            EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
            EXPECT_NE(resp.find("OK"), std::string::npos);
        }

        // Kill exits the armed accept loop via the listener shutdown guard
        //
        serverHandle.Kill();
        for (int i = 0; i < 200 && !serverReturned; i++)
        {
            ctx->Yield(true);
        }
        EXPECT_TRUE(serverReturned);
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
    });
}

// -------------------------------------------------------------------------------------
// RecvPolicy::PollFirst — the client turnaround shape (send request, await response)
// -------------------------------------------------------------------------------------

TEST(HttpClientTest, PollFirstRoundtrip)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        coop::http::PlaintextTransport transport(client, coop::http::RecvPolicy::PollFirst);
        auto conn = ctx->Allocate<HttpClient>(CLIENT_EXTRA,
            transport, "origin.example");

        EXPECT_TRUE(conn->Get("/x"));
        RecvAll(server);
        SendResponse(server,
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

        auto* resp = conn->GetResponseLine();
        ASSERT_NE(resp, nullptr);
        EXPECT_EQ(resp->status, 200);
        conn->SkipHeaders();
        std::string body;
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
        }
        EXPECT_EQ(body, "ok");
    });
}

// -------------------------------------------------------------------------------------
// Pbuf-mode parsing: kernel-selected chunks through the ParseWindow seam
// -------------------------------------------------------------------------------------

namespace
{

// Run fn on a cooperator whose uring carries a provided buffer ring of `entries` bufs
// of `bufSize` bytes — small sizes force multi-chunk requests through the reassembly
// path.
//
void RunWithBufferRing(uint32_t entries, uint32_t bufSize,
                       std::function<void(coop::Context*)> fn)
{
    coop::CooperatorConfiguration cfg;
    cfg.uring.bufferRingEntries = entries;
    cfg.uring.bufferRingBufSize = bufSize;

    coop::Cooperator cooperator(cfg);
    coop::Thread thread(&cooperator);

    cooperator.SubmitSync([&](coop::Context* ctx)
    {
        // Keep shutdown outside the assertion's return path so an unavailable pool
        // reports its prerequisite failure without crashing or stranding the thread.
        //
        [&]
        {
            ASSERT_NE(coop::GetUring()->GetBufferRing(), nullptr)
                << "provided buffer-ring registration failed";
            fn(ctx);
        }();
        cooperator.Shutdown();
    });
}

} // end anonymous namespace

TEST(HttpPbufTest, WholeRequestInOneChunk)
{
    RunWithBufferRing(16, 4096, [](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        ASSERT_NE(uring->GetBufferRing(), nullptr);

        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendString(client,
            "GET /bucket/key?v=1 HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "x-amz-test: abc\r\n"
            "\r\n");

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());

        coop::io::RecvSource source(ctx, server, uring->GetBufferRing());
        conn->AttachRecvSource(&source);

        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->method, "GET");
        EXPECT_EQ(req->path, "/bucket/key");
        EXPECT_EQ(req->target, "/bucket/key?v=1");

        bool sawHeader = false;
        while (auto* name = conn->NextHeaderName())
        {
            if (strcasecmp(name, "x-amz-test") == 0)
            {
                auto* v = conn->ReadHeaderValue();
                ASSERT_NE(v, nullptr);
                EXPECT_EQ(std::string_view(static_cast<const char*>(v->data), v->size),
                          "abc");
                sawHeader = true;
            }
            else
            {
                conn->SkipHeaderValue();
            }
        }
        EXPECT_TRUE(sawHeader);
        EXPECT_TRUE(conn->Send(200, "text/plain", "OK"));

        std::string resp = RecvAll(client);
        EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
    });
}

TEST(HttpPbufTest, RequestSplitAcrossChunks)
{
    // 128-byte pool buffers force the head across several chunks — the staging
    // reassembly path — while the 2KB staging buffer absorbs it comfortably.
    //
    RunWithBufferRing(32, 128, [](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        std::string request =
            "PUT /obj HTTP/1.1\r\n"
            "Host: bucket.s3.example\r\n"
            "Authorization: AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20260804/us-east-1/"
            "s3/aws4_request, SignedHeaders=host;x-amz-date, Signature="
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\r\n"
            "x-amz-date: 20260804T000000Z\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        SendString(client, request.c_str());

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());

        coop::io::RecvSource source(ctx, server, uring->GetBufferRing());
        conn->AttachRecvSource(&source);

        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->method, "PUT");

        bool sawAuth = false;
        while (auto* name = conn->NextHeaderName())
        {
            if (strcasecmp(name, "Authorization") == 0)
            {
                std::string value;
                // Header values can arrive in pieces at this pool geometry
                //
                for (auto* v = conn->ReadHeaderValue(); v != nullptr;
                     v = v->complete ? nullptr : conn->ReadHeaderValue())
                {
                    value.append(static_cast<const char*>(v->data), v->size);
                    if (v->complete) break;
                }
                EXPECT_NE(value.find("Signature=0123456789abcdef"), std::string::npos);
                sawAuth = true;
            }
            else
            {
                conn->SkipHeaderValue();
            }
        }
        EXPECT_TRUE(sawAuth);

        std::string body;
        while (auto* chunk = conn->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
        }
        EXPECT_EQ(body, "hello");
    });
}

TEST(HttpPbufTest, BodyStreamsAndKeepAliveReuses)
{
    RunWithBufferRing(16, 512, [](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        std::string body = PatternBody(4096);
        std::string request =
            "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 4096\r\n\r\n" + body +
            "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
        coop::io::SendAll(client, request.data(), request.size());

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());

        coop::io::RecvSource source(ctx, server, uring->GetBufferRing());
        conn->AttachRecvSource(&source);

        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->path, "/a");

        std::string got;
        while (auto* chunk = conn->ReadBody())
        {
            got.append(static_cast<const char*>(chunk->data), chunk->size);
        }
        EXPECT_EQ(got.size(), body.size());
        EXPECT_EQ(got, body);

        // The pipelined second request survives the window transitions
        //
        conn->Reset();
        req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->path, "/b");
    });
}

TEST(HttpPbufTest, RunServerFullModernPath)
{
    coop::http::ServerHandle control;
    bool serverReturned = false;
    // Multishot accept + pbuf-chunk parsing together — the modern server data path
    //
    RunWithBufferRing(64, 2048, [&](coop::Context* ctx)
    {
        coop::Context::Handle serverHandle;
        ctx->GetCooperator()->Spawn(
            {.priority = 0, .stackSize = 65536},
            [&](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.multishotAccept = true;
            config.pbufRecv = true;
            config.name = "TestModernServer";
            config.handler = &OkHandler;
            coop::http::RunServer(serverCtx, config);
            serverReturned = true;
        }, &serverHandle);

        const auto port = control.Port();
        ASSERT_NE(port, 0);

        for (int round = 0; round < 3; round++)
        {
            int cfd = socket(AF_INET, SOCK_STREAM, 0);
            ASSERT_GE(cfd, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            ASSERT_EQ(connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0)
                << strerror(errno);

            coop::io::Descriptor client(cfd, coop::GetUring());
            const char* req = "GET /ok HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
            ASSERT_GT(coop::io::SendAll(client, req, strlen(req)), 0);

            std::string resp = RecvAll(client, 4096);
            EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos) << "round " << round;
        }

        serverHandle.Kill();
        for (int i = 0; i < 200 && !serverReturned; i++)
        {
            ctx->Yield(true);
        }
        EXPECT_TRUE(serverReturned);
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
    });
}

// A keep-alive client that disconnects idle must END its server connection context —
// not spin it hot on instant zero-byte reads (which monopolizes the cooperator and
// starves every other connection). The second connection here starves if the first
// one's EOF spins.
//
TEST(HttpTest, KeepAliveClientDisconnectDoesNotSpin)
{
    coop::http::ServerHandle control;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::Context::Handle serverHandle;
        ctx->GetCooperator()->Spawn(
            {.priority = 0, .stackSize = 65536},
            [&](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.name = "EofSpinServer";
            config.handler = &OkHandler;
            coop::http::RunServer(serverCtx, config);
        }, &serverHandle);

        const auto port = control.Port();
        ASSERT_NE(port, 0);

        auto roundtrip = [&](bool close) -> bool
        {
            int cfd = socket(AF_INET, SOCK_STREAM, 0);
            if (cfd < 0) return false;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            {
                ::close(cfd);
                return false;
            }
            coop::io::Descriptor client(cfd, coop::GetUring());
            const char* req = close
                ? "GET /ok HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"
                : "GET /ok HTTP/1.1\r\nHost: t\r\n\r\n";
            if (coop::io::SendAll(client, req, strlen(req)) <= 0) return false;
            std::string resp = RecvAll(client, 2048);
            return resp.find("200 OK") != std::string::npos;
        };

        // Keep-alive roundtrip, then the client just disconnects (Descriptor dtor)
        //
        ASSERT_TRUE(roundtrip(false));

        // Give the EOF a few scheduler passes to land on the server side
        //
        for (int i = 0; i < 20; i++) ctx->Yield(true);

        // A spinning EOF loop would monopolize the cooperator and starve this one
        //
        EXPECT_TRUE(roundtrip(true));

        serverHandle.Kill();
        for (int i = 0; i < 100; i++) ctx->Yield(true);
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
    });
}

// A peer that closes mid-header-block (an incomplete request, then EOF) must not spin
// the parser: AdvanceToPhase used to loop forever when SkipHeaders hit EOF without
// advancing the phase. Surfaced by graceful drain's SHUT_RD mid-parse.
//
TEST(HttpTest, MidHeaderEofDoesNotSpinParser)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        // Complete request line, one header, then close — no terminating blank line.
        //
        SendString(client, "GET /x HTTP/1.1\r\nHost: t\r\n");
        client.Close();
        sp.fds[0] = -1;

        coop::http::PlaintextTransport transport(server);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            transport, ctx, ctx->GetCooperator());

        auto* req = conn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->path, "/x");

        // SkipBody advances through the truncated header block; must terminate, not spin.
        //
        conn->SkipBody();
        EXPECT_EQ(conn->ReadBody(), nullptr);   // no body; reached DONE cleanly
    });
}

// =====================================================================================
// Framing enforcement
//
// Every field these tests feed is one a peer chooses. Each case pairs the malformed
// framing that must fail the connection with the well-formed framing that must still
// parse, so a rule that rejects everything is as visible as one that rejects nothing.
// =====================================================================================

namespace
{

// Feed `request` to a connection whose recv buffer is `recvBufSize`, so a refill lands at
// a chosen byte of the request. Returns the parsed connection through `fn`.
//
template<typename Fn>
void WithRequest(coop::Context* ctx, const std::string& request, size_t recvBufSize,
                 Fn&& fn)
{
    SocketPair sp;
    auto* uring = coop::GetUring();
    coop::io::Descriptor client(sp.fds[0], uring);
    coop::io::Descriptor server(sp.fds[1], uring);

    coop::io::SendAll(client, request.data(), request.size());

    coop::http::PlaintextTransport transport(server);
    auto conn = ctx->Allocate<HttpConn>(
        HttpConn::ExtraBytes(recvBufSize, HttpConn::DEFAULT_SEND_BUFFER_SIZE),
        transport, ctx, ctx->GetCooperator(),
        recvBufSize, HttpConn::DEFAULT_SEND_BUFFER_SIZE,
        std::chrono::milliseconds(200));

    fn(*conn, client);
}

// Read a whole body, however many chunks it arrives in.
//
std::string DrainBody(coop::http::ConnectionBase& conn)
{
    std::string body;
    while (auto* chunk = conn.ReadBody())
    {
        body.append(static_cast<const char*>(chunk->data), chunk->size);
    }
    return body;
}

std::string ChunkedRequest(const std::string& chunks)
{
    return "POST /upload HTTP/1.1\r\n"
           "Transfer-Encoding: chunked\r\nHost: example.test\r\n"
           "\r\n" + chunks;
}

} // end anonymous namespace

// `c & 0xF` decodes 'A' as 1 and 'a' as 1 — every letter names a size the sender never
// wrote. The size line is hex or it is nothing.
//
TEST(HttpFramingTest, ChunkSizeDecodesHexDigits)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx, ChunkedRequest(
            "A\r\n0123456789\r\n"
            "1a\r\nabcdefghijklmnopqrstuvwxyz\r\n"
            "0\r\n\r\n"), 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            EXPECT_EQ(DrainBody(conn), "0123456789abcdefghijklmnopqrstuvwxyz");
            EXPECT_EQ(conn.ParseError(), 0);
        });
    });
}

TEST(HttpFramingTest, ChunkSizeRejectsNonHex)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx, ChunkedRequest("ZZ\r\nignored\r\n0\r\n\r\n"), 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor& client)
        {
            EXPECT_EQ(DrainBody(conn), "");
            EXPECT_EQ(conn.ParseError(), 400);
            EXPECT_FALSE(conn.KeepAlive());

            // The peer is told, once, on the wire.
            //
            EXPECT_NE(RecvAll(client).find("400 Bad Request"), std::string::npos);
        });
    });
}

// A size line longer than 16 hex digits cannot be held in a size_t: without the digit
// bound it wraps, and the wrapped value that reads as 0 is the terminal chunk — a
// truncated body handed over as a complete one.
//
TEST(HttpFramingTest, ChunkSizeRejectsSeventeenHexDigits)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx, ChunkedRequest("10000000000000000\r\nx\r\n0\r\n\r\n"), 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            EXPECT_EQ(DrainBody(conn), "");
            EXPECT_EQ(conn.ParseError(), 400);
        });
    });
}

TEST(HttpFramingTest, ChunkSizeRejectsAboveCeiling)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx, ChunkedRequest("ffffffffffffffff\r\n"), 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            EXPECT_EQ(DrainBody(conn), "");
            EXPECT_EQ(conn.ParseError(), 413);
        });
    });
}

// Chunk extensions are the one thing that may follow the digits, and they end at the CRLF.
//
TEST(HttpFramingTest, ChunkSizeAcceptsExtensions)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx, ChunkedRequest("5;name=value\r\nHello\r\n0\r\n\r\n"), 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            EXPECT_EQ(DrainBody(conn), "Hello");
            EXPECT_EQ(conn.ParseError(), 0);
        });
    });
}

// The two bytes after a chunk's data are its CRLF. Stepping over them unread lets the
// peer put the head of a second request there.
//
TEST(HttpFramingTest, ChunkTerminatorMustBeCrlf)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx, ChunkedRequest("5\r\nHelloXX0\r\n\r\n"), 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            DrainBody(conn);
            EXPECT_EQ(conn.ParseError(), 400);
        });
    });
}

// Trailers and the blank line closing them belong to this request. Left buffered, they
// are read as the next one — and the peer wrote them.
//
TEST(HttpFramingTest, ChunkedTrailersConsumedBeforeKeepAlive)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx, ChunkedRequest(
            "5\r\nHello\r\n"
            "0\r\n"
            "X-Checksum: deadbeef\r\n"
            "\r\n"
            "GET /next HTTP/1.1\r\nHost: t\r\n\r\n"), 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            EXPECT_EQ(DrainBody(conn), "Hello");
            EXPECT_EQ(conn.ParseError(), 0);

            conn.SkipBody();
            ASSERT_TRUE(conn.KeepAlive());
            conn.Reset();

            auto* next = conn.GetRequestLine();
            ASSERT_NE(next, nullptr);
            EXPECT_EQ(next->method, "GET");
            EXPECT_EQ(next->path, "/next");
        });
    });
}

// The recv buffer ends on the CR of the Host line, so the LF arrives in the next refill.
// Treating the lone CR as the terminator leaves the parser on it, and the following scan
// reads that CR and its LF as the blank line ending the block — hiding Content-Length and
// leaving the rest of the headers to be parsed as a second request.
//
TEST(HttpFramingTest, HeaderTerminatorSplitAcrossRefillSeesAllHeaders)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            "GET /x HTTP/1.1\r\n"
            "Host: t\r\n"
            "X-A: 1\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello", 25,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            auto* req = conn.GetRequestLine();
            ASSERT_NE(req, nullptr);

            std::string names;
            while (const char* name = conn.NextHeaderName())
            {
                names += name;
                names += ";";
                conn.SkipHeaderValue();
            }
            EXPECT_EQ(names, "Host;X-A;Content-Length;");
            EXPECT_EQ(conn.ContentLength(), 5);
            EXPECT_EQ(DrainBody(conn), "hello");
        });
    });
}

// The refill lands mid-digits. Restarting the decode from whatever survived the refill
// reads "1234" as 34 — a body length neither endpoint declared.
//
TEST(HttpFramingTest, ContentLengthSplitAcrossRefillDecodesWhole)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            "POST /u HTTP/1.1\r\n"
            "Content-Length: 1234\r\nHost: example.test\r\n"
            "\r\n", 36,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            EXPECT_EQ(conn.ContentLength(), 1234);
            EXPECT_EQ(conn.ParseError(), 0);
        });
    });
}

// Content-Length is 1*DIGIT. Skipping the non-digits reads "1abc0" as 10.
//
TEST(HttpFramingTest, ContentLengthRejectsNonDigits)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            "POST /u HTTP/1.1\r\nContent-Length: 1abc0\r\nHost: example.test\r\n\r\n", 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            conn.SkipHeaders();
            EXPECT_EQ(conn.ParseError(), 400);
        });
    });
}

// Leading zeros are digits: 1*DIGIT admits them and they name the same length.
//
TEST(HttpFramingTest, ContentLengthAcceptsLeadingZeros)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            "POST /u HTTP/1.1\r\nContent-Length: 007\r\nHost: example.test\r\n\r\nseven!!", 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            EXPECT_EQ(conn.ContentLength(), 7);
            EXPECT_EQ(conn.ParseError(), 0);
        });
    });
}

TEST(HttpFramingTest, ContentLengthRejectsTwentyDigits)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            "POST /u HTTP/1.1\r\nContent-Length: 99999999999999999999\r\nHost: example.test\r\n\r\n", 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            conn.SkipHeaders();
            EXPECT_EQ(conn.ParseError(), 400);
        });
    });
}

// Two different lengths leave the message with no single boundary; the same length twice
// is one answer said twice.
//
TEST(HttpFramingTest, DuplicateContentLengthRejectedWhenValuesDiffer)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            "POST /u HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\nHost: example.test\r\n\r\nhello",
            2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            conn.SkipHeaders();
            EXPECT_EQ(conn.ParseError(), 400);
        });
    });
}

TEST(HttpFramingTest, DuplicateContentLengthAcceptedWhenValuesAgree)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            "POST /u HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\nHost: example.test\r\n\r\nhello",
            2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            EXPECT_EQ(conn.ContentLength(), 5);
            EXPECT_EQ(conn.ParseError(), 0);
            EXPECT_EQ(DrainBody(conn), "hello");
        });
    });
}

// Content-Length and Transfer-Encoding are two answers to where the body ends. Whichever
// one a reader picks, another reader picking the other finds a request nobody sent.
//
TEST(HttpFramingTest, ContentLengthWithTransferEncodingRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            "POST /u HTTP/1.1\r\n"
            "Content-Length: 6\r\n"
            "Transfer-Encoding: chunked\r\nHost: example.test\r\n"
            "\r\n"
            "0\r\n\r\nGET /smuggled HTTP/1.1\r\nHost: t\r\n\r\n", 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            conn.SkipHeaders();
            EXPECT_EQ(conn.ParseError(), 400);
            EXPECT_FALSE(conn.KeepAlive());

            // Reset must not turn the smuggled bytes into the next request: the parser
            // remains terminal rather than starting a request the peer hid behind a
            // body boundary nobody agreed on. Terminal accessors return failure, not a
            // cached borrowed request line whose storage may already have changed.
            //
            EXPECT_FALSE(conn.Reset());
            EXPECT_EQ(conn.ParseError(), 400);
            EXPECT_EQ(conn.Error(), -EPROTO);
            EXPECT_EQ(conn.GetRequestLine(), nullptr);
        });
    });
}

// `chunked` is the only transfer coding this parser can frame. A prefix match accepts
// "chunkedfoo", which is not it.
//
TEST(HttpFramingTest, UnknownTransferEncodingRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            "POST /u HTTP/1.1\r\nTransfer-Encoding: chunkedfoo\r\nHost: example.test\r\n\r\n", 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            conn.SkipHeaders();
            EXPECT_EQ(conn.ParseError(), 400);
        });
    });
}

namespace
{

std::string ManyHeaders(int count)
{
    std::string request = "GET /x HTTP/1.1\r\nHost: example.test\r\n";
    for (int i = 1; i < count; i++)
    {
        request += "X-" + std::to_string(i) + ": v\r\n";
    }
    return request + "\r\n";
}

} // end anonymous namespace

TEST(HttpFramingTest, HeaderCountAtLimitAccepted)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx, ManyHeaders(coop::http::ConnectionBase::MAX_HEADER_COUNT), 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            conn.SkipHeaders();
            EXPECT_EQ(conn.ParseError(), 0);
        });
    });
}

// The recv buffer bounds one header line, not the block: the parser compacts and refills
// as it scans, so without a count and a byte budget a peer streams headers indefinitely.
//
TEST(HttpFramingTest, HeaderCountOverLimitRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithRequest(ctx,
            ManyHeaders(coop::http::ConnectionBase::MAX_HEADER_COUNT + 1), 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            conn.SkipHeaders();
            EXPECT_EQ(conn.ParseError(), 431);
        });
    });
}

TEST(HttpFramingTest, CumulativeHeaderBytesOverLimitRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string request = "GET /x HTTP/1.1\r\nX-Big: "
                            + std::string(coop::http::ConnectionBase::MAX_HEADER_BYTES + 1,
                                          'a')
                            + "\r\n\r\n";
        WithRequest(ctx, request, 2048,
            [](coop::http::ConnectionBase& conn, coop::io::Descriptor&)
        {
            ASSERT_NE(conn.GetRequestLine(), nullptr);
            conn.SkipHeaders();
            EXPECT_EQ(conn.ParseError(), 431);
        });
    });
}

namespace
{
void RunHttpSendScenario(test::SendScenario scenario, bool sendfile = false)
{
    for (auto kind : {test::SendTransport::Plaintext, test::SendTransport::MemoryTls,
                      test::SendTransport::SocketTls})
    {
        SCOPED_TRACE(static_cast<int>(kind));
        test::RunSendScenario(kind, scenario,
            [sendfile](auto transport, coop::Context* ctx, const std::string& payload,
               coop::time::Interval timeout, bool flush, coop::CountdownLatch& staged)
            {
                using Connection = coop::http::Connection<decltype(transport)>;
                auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
                    transport, ctx, ctx->GetCooperator(),
                    Connection::DEFAULT_BUFFER_SIZE,
                    Connection::DEFAULT_SEND_BUFFER_SIZE, timeout);
                if (sendfile)
                {
                    coop::io::Descriptor file(memfd_create("http-send", MFD_CLOEXEC));
                    EXPECT_GE(file.m_fd, 0);
                    if (file.m_fd < 0) return false;
                    int written = coop::io::WriteKill(file, payload.data(), payload.size(),
                        0, 0, std::chrono::seconds(1));
                    EXPECT_EQ(written, static_cast<int>(payload.size()));
                    staged.Arrive();
                    if (written != static_cast<int>(payload.size())) return false;
                    return conn->SendHeaders(200, "text/plain", payload.size())
                        && conn->Sendfile(file.m_fd, 0, payload.size());
                }
                staged.Arrive();
                if (flush)
                    return conn->BeginChunked(200, "text/plain")
                        && conn->EndChunked(payload.data(), payload.size());
                return conn->Send(200, "text/plain", payload);
            });
    }
}
} // namespace

TEST(HttpTest, StalledSendTimesOut)
{
    RunHttpSendScenario(test::SendScenario::Stalled);
}

TEST(HttpTest, SteadySendRefreshesInactivityBudget)
{
    RunHttpSendScenario(test::SendScenario::Steady);
}

TEST(HttpTest, KillWakesFinalFlushAndSiblingRuns)
{
    RunHttpSendScenario(test::SendScenario::KilledFlush);
}

TEST(HttpTest, SendObservesContextDeadline)
{
    RunHttpSendScenario(test::SendScenario::Deadline);
}

TEST(HttpTest, ShortWritesAdvanceByActualProgress)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        test::SendScript script;
        coop::io::Descriptor descriptor(coop::io::borrowed, -1);
        test::ShortWriteTransport transport{&script, &descriptor};
        using Connection = coop::http::Connection<test::ShortWriteTransport>;
        auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
            transport, ctx, ctx->GetCooperator());
        std::string payload(40000, 'x');
        ASSERT_TRUE(conn->SendRawBytes(payload.data(), payload.size()));
        EXPECT_EQ(script.output, payload);
        EXPECT_GT(script.calls, 1u);
        EXPECT_LE(script.largest, 16384u);
    });
}

TEST(HttpTest, RepeatedSendsYieldToSibling)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        test::SendScript script;
        script.limit = SIZE_MAX;
        script.output.reserve(3 * 1024 * 1024);
        coop::io::Descriptor descriptor(coop::io::borrowed, -1);
        test::ShortWriteTransport transport{&script, &descriptor};
        using Connection = coop::http::Connection<test::ShortWriteTransport>;
        auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
            transport, ctx, ctx->GetCooperator());
        bool siblingRan = false;
        coop::CountdownLatch joined(2);
        ASSERT_TRUE(coop::Spawn([&](coop::Context* child)
        {
            child->Yield();
            siblingRan = true;
            joined.Arrive();
        }));
        std::string payload(4096, 'x');
        ASSERT_TRUE(coop::Spawn([&](coop::Context*)
        {
            for (size_t i = 0; i < 600; ++i)
                EXPECT_TRUE(conn->SendRawBytes(payload.data(), payload.size()));
            EXPECT_TRUE(siblingRan);
            joined.Arrive();
        }));
        joined.Wait(ctx);
        EXPECT_TRUE(siblingRan);
    });
}

TEST(HttpTest, FinalChunkFlushTimesOut)
{
    RunHttpSendScenario(test::SendScenario::FinalFlush);
}

TEST(HttpTest, StalledFileSendTimesOut)
{
    RunHttpSendScenario(test::SendScenario::Stalled, true);
}

TEST(HttpTest, KillWakesFileSendAndSiblingRuns)
{
    RunHttpSendScenario(test::SendScenario::KilledFile, true);
}

TEST(HttpTest, WritableSendObservesExpiredContextDeadline)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        test::SendScript script;
        script.limit = SIZE_MAX;
        coop::io::Descriptor descriptor(coop::io::borrowed, -1);
        test::ShortWriteTransport transport{&script, &descriptor};
        using Connection = coop::http::Connection<test::ShortWriteTransport>;
        auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
            transport, ctx, ctx->GetCooperator());
        ctx->SetDeadline(coop::time::MonotonicMicros() - 1);
        EXPECT_TRUE(conn->SendRawBytes("x", 1));
        EXPECT_EQ(ctx->WhyKilled(), coop::KillCause::Deadline);
        EXPECT_EQ(script.calls, 0u);
        EXPECT_EQ(script.tryCalls, 1u);
    });
}

TEST(HttpTest, ChildWriterObservesItsOwnCancellation)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        test::SendScript script;
        script.limit = SIZE_MAX;
        script.killOnWrite = true;
        coop::io::Descriptor descriptor(coop::io::borrowed, -1);
        using Connection = coop::http::Connection<test::ShortWriteTransport>;
        auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
            test::ShortWriteTransport{&script, &descriptor}, ctx, ctx->GetCooperator());
        coop::CountdownLatch joined(1);
        ASSERT_TRUE(coop::Spawn([&](coop::Context*)
        {
            std::string payload(49152, 'x');
            EXPECT_FALSE(conn->SendRawBytes(payload.data(), payload.size()));
            EXPECT_TRUE(conn->SendError());
            joined.Arrive();
        }));
        joined.Wait(ctx);
        EXPECT_FALSE(ctx->IsKilled());
        EXPECT_EQ(script.calls, 1u);
        EXPECT_EQ(script.tryCalls, 0u);
        EXPECT_EQ(script.output.size(), 16384u);
    });
}

TEST(HttpTest, CanceledOversizedWriteDoesNotSendAPrefix)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        test::SendScript script;
        coop::io::Descriptor descriptor(coop::io::borrowed, -1);
        using Connection = coop::http::Connection<test::ShortWriteTransport>;
        auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
            test::ShortWriteTransport{&script, &descriptor}, ctx, ctx->GetCooperator());
        std::string payload(32768, 'x');
        ctx->KillSelf(coop::KillCause::Kill);
        EXPECT_FALSE(conn->SendRawBytes(payload.data(), payload.size()));
        EXPECT_TRUE(script.output.empty());
        EXPECT_EQ(script.calls, 0u);
        EXPECT_EQ(script.tryCalls, 0u);
    });
}

TEST(HttpTest, CanceledUnstartedFileSendPreservesFinalWrite)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        test::SendScript script;
        coop::io::Descriptor descriptor(coop::io::borrowed, -1);
        using Connection = coop::http::Connection<test::ShortWriteTransport>;
        auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
            test::ShortWriteTransport{&script, &descriptor}, ctx, ctx->GetCooperator());
        ctx->KillSelf(coop::KillCause::Kill);
        EXPECT_FALSE(conn->Sendfile(-1, 0, 32768));
        EXPECT_FALSE(conn->SendError());
        EXPECT_TRUE(conn->SendRawBytes("goodbye", 7));
        EXPECT_FALSE(conn->SendRawBytes("again", 5));
        EXPECT_EQ(script.output, "goodbye");
        EXPECT_EQ(script.calls, 0u);
        EXPECT_EQ(script.tryCalls, 1u);
    });
}

TEST(HttpTest, CanceledUnstartedFileSendDoesNotFlushBufferedResponse)
{
    for (bool deadline : {false, true})
        test::RunInCooperator([&](coop::Context* ctx)
        {
            test::SendScript script;
            coop::io::Descriptor descriptor(coop::io::borrowed, -1);
            using Connection = coop::http::Connection<test::ShortWriteTransport>;
            auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
                test::ShortWriteTransport{&script, &descriptor}, ctx, ctx->GetCooperator());
            ASSERT_TRUE(conn->BeginResponse(503, "Service Unavailable"));
            ASSERT_TRUE(conn->AppendHeader("Content-Length", size_t(0)));
            if (deadline) ctx->SetDeadline(coop::time::MonotonicMicros() - 1);
            else ctx->KillSelf(coop::KillCause::Kill);
            EXPECT_FALSE(conn->Sendfile(-1, 0, 32768));
            EXPECT_FALSE(conn->SendError());
            EXPECT_TRUE(script.output.empty());
            EXPECT_EQ(script.tryCalls, 0u);
            EXPECT_TRUE(conn->EndHeaders());
            EXPECT_EQ(script.calls, 0u);
            EXPECT_EQ(script.tryCalls, 1u);
            EXPECT_EQ(script.output.find("HTTP/1.1 503 Service Unavailable\r\n"), 0u);
            EXPECT_NE(script.output.find("Content-Length: 0\r\n"), std::string::npos);
            EXPECT_EQ(script.output.substr(script.output.size() - 4), "\r\n\r\n");
        });
}

TEST(HttpTest, CancellationAllowsExactlyOneTruthfulImmediateWrite)
{
    for (size_t limit : {size_t(3), SIZE_MAX})
        test::RunInCooperator([&](coop::Context* ctx)
        {
            test::SendScript script;
            script.tryLimit = limit;
            coop::io::Descriptor descriptor(coop::io::borrowed, -1);
            using Connection = coop::http::Connection<test::ShortWriteTransport>;
            auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
                test::ShortWriteTransport{&script, &descriptor}, nullptr, ctx->GetCooperator());
            ctx->KillSelf(coop::KillCause::Kill);
            EXPECT_EQ(conn->SendRawBytes("goodbye", 7), limit >= 7);
            EXPECT_FALSE(conn->SendRawBytes("again", 5));
            EXPECT_EQ(script.calls, 0u);
            EXPECT_EQ(script.tryCalls, 1u);
            EXPECT_EQ(script.output, limit >= 7 ? "goodbye" : "goo");
        });
}

TEST(HttpTest, TrickleReaderCannotExtendAbsoluteDeadline)
{
    RunHttpSendScenario(test::SendScenario::TrickleDeadline);
}

TEST(HttpTest, TimedFileSendPreservesRecordSizedProgress)
{
    for (auto kind : {test::SendTransport::Plaintext, test::SendTransport::MemoryTls,
                      test::SendTransport::SocketTls})
        test::RunInCooperator([&](coop::Context* ctx)
        {
            ctx->SetDeadlineIn(std::chrono::seconds(2));
            auto pair = std::make_unique<test::SendPair>(kind);
            int capacity = 65536;
            ASSERT_EQ(setsockopt(pair->server.m_fd, SOL_SOCKET, SO_SNDBUF,
                                &capacity, sizeof(capacity)), 0);
            coop::io::Descriptor file(memfd_create("sendfile-record", MFD_CLOEXEC));
            ASSERT_GE(file.m_fd, 0);
            std::string payload(16384, 'f');
            ASSERT_EQ(coop::io::WriteKill(file, payload.data(), payload.size(), 0), 16384);
            int n = kind == test::SendTransport::Plaintext
                ? coop::io::SendfileKill(pair->server, file.m_fd, 0, payload.size(),
                                        std::chrono::milliseconds(150))
                : coop::io::ssl::SendfileKill(*pair->serverTls, file.m_fd, 0, payload.size(),
                                             std::chrono::milliseconds(150));
            ASSERT_GT(n, 4096);
            EXPECT_LE(n, 16384);
            if (kind != test::SendTransport::Plaintext) EXPECT_EQ(n, 16384);
            std::string actual;
            char buffer[16384];
            while (actual.size() < static_cast<size_t>(n))
            {
                int got = pair->Recv(buffer, sizeof(buffer));
                ASSERT_GT(got, 0);
                actual.append(buffer, got);
            }
            EXPECT_EQ(actual, payload.substr(0, n));
        }, {.stackSize = 65536});
}

namespace
{

void CheckTimedFileWithDefaultSigpipe(test::SendTransport kind, bool resetPeer = false,
                                     int signalState = 0)
{
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    ASSERT_EQ(sigaction(SIGPIPE, &action, nullptr), 0);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    ASSERT_EQ(pthread_sigmask(SIG_UNBLOCK, &signals, nullptr), 0);
    test::RunInCooperator([&](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(2));
        auto pair = std::make_unique<test::SendPair>(kind);
        if (kind == test::SendTransport::Ktls) ASSERT_TRUE(pair->serverTls->m_ktlsTx);
        if (signalState)
        {
            ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &signals, nullptr), 0);
            if (signalState == 2) ASSERT_EQ(raise(SIGPIPE), 0);
        }
        if (resetPeer)
        {
            linger reset{1, 0};
            ASSERT_EQ(setsockopt(pair->client.m_fd, SOL_SOCKET, SO_LINGER,
                                &reset, sizeof(reset)), 0);
        }
        ASSERT_EQ(pair->client.Close(), 0);
        if (resetPeer)
        {
            char byte;
            ASSERT_EQ(coop::io::RecvKill(pair->server, &byte, 1, 0,
                                         std::chrono::milliseconds(150)), -ECONNRESET);
        }
        else
        {
            // Also cover an output half closed before the file send begins.
            ASSERT_EQ(shutdown(pair->server.m_fd, SHUT_WR), 0);
        }
        coop::io::Descriptor file(memfd_create("sendfile-closed-output", MFD_CLOEXEC));
        ASSERT_GE(file.m_fd, 0);
        constexpr char payload[] = "file response";
        ASSERT_EQ(coop::io::WriteKill(file, payload, sizeof(payload), 0), sizeof(payload));
        int result = kind == test::SendTransport::Plaintext
            ? coop::io::SendfileKill(pair->server, file.m_fd, 0, sizeof(payload),
                                     std::chrono::milliseconds(150))
            : coop::io::ssl::SendfileKill(*pair->serverTls, file.m_fd, 0, sizeof(payload),
                                          std::chrono::milliseconds(150));
        EXPECT_EQ(result, -EPIPE);
        sigset_t current, pending;
        EXPECT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &current), 0);
        EXPECT_EQ(sigismember(&current, SIGPIPE), signalState != 0);
        EXPECT_EQ(sigpending(&pending), 0);
        EXPECT_EQ(sigismember(&pending, SIGPIPE), signalState == 2);
    }, {.stackSize = 65536});
}

} // namespace

TEST(HttpFileSendDeathTest, PlaintextClosedOutputReturnsEpipeWithDefaultSigpipe)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ASSERT_EXIT({
        CheckTimedFileWithDefaultSigpipe(test::SendTransport::Plaintext);
        CheckTimedFileWithDefaultSigpipe(test::SendTransport::Plaintext, false, 1);
        CheckTimedFileWithDefaultSigpipe(test::SendTransport::Plaintext, false, 2);
        _exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), "");
}

TEST(HttpFileSendDeathTest, KtlsClosedOutputReturnsEpipeWithDefaultSigpipe)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    bool activated = false;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(2));
        auto pair = std::make_unique<test::SendPair>(test::SendTransport::Ktls);
        activated = pair->serverTls->m_ktlsTx;
    }, {.stackSize = 65536});
    if (!activated) GTEST_SKIP() << "kernel/OpenSSL did not activate kTLS";
    ASSERT_EXIT({
        CheckTimedFileWithDefaultSigpipe(test::SendTransport::Ktls);
        CheckTimedFileWithDefaultSigpipe(test::SendTransport::Ktls, true);
        CheckTimedFileWithDefaultSigpipe(test::SendTransport::Ktls, true, 1);
        CheckTimedFileWithDefaultSigpipe(test::SendTransport::Ktls, true, 2);
        _exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), "");
}

TEST(HttpTest, KtlsTimedFileAndCancellationPaths)
{
    bool activated = false;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(2));
        auto pair = std::make_unique<test::SendPair>(test::SendTransport::Ktls);
        activated = pair->serverTls->m_ktlsTx && pair->clientTls->m_ktlsRx;
    }, {.stackSize = 65536});
    if (!activated) GTEST_SKIP() << "kernel/OpenSSL did not activate kTLS";
    for (auto scenario : {test::SendScenario::Stalled, test::SendScenario::KilledFile,
                          test::SendScenario::Steady, test::SendScenario::TrickleDeadline})
        test::RunSendScenario(test::SendTransport::Ktls, scenario,
            [](auto transport, coop::Context* child, const std::string& payload,
               coop::time::Interval timeout, bool, coop::CountdownLatch& staged)
            {
                coop::io::Descriptor file(memfd_create("ktls-sendfile", MFD_CLOEXEC));
                EXPECT_GE(file.m_fd, 0);
                EXPECT_EQ(coop::io::WriteKill(file, payload.data(), payload.size(), 0),
                          static_cast<int>(payload.size()));
                staged.Arrive();
                using Connection = coop::http::Connection<decltype(transport)>;
                auto conn = child->Allocate<Connection>(Connection::ExtraBytes(),
                    transport, child, child->GetCooperator(), Connection::DEFAULT_BUFFER_SIZE,
                    Connection::DEFAULT_SEND_BUFFER_SIZE, timeout);
                return conn->Sendfile(file.m_fd, 0, payload.size());
            });
}

TEST(HttpTest, AmbiguousCancelledSendIsNeverReplayed)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        test::SendScript script;
        script.killOnWrite = true;
        script.cancelAfterWrite = true;
        coop::io::Descriptor descriptor(coop::io::borrowed, -1);
        using Connection = coop::http::Connection<test::ShortWriteTransport>;
        auto conn = ctx->Allocate<Connection>(Connection::ExtraBytes(),
            test::ShortWriteTransport{&script, &descriptor}, ctx, ctx->GetCooperator());
        std::string payload(1000, 'x');
        EXPECT_FALSE(conn->SendRawBytes(payload.data(), payload.size()));
        EXPECT_EQ(script.calls, 1u);
        EXPECT_EQ(script.tryCalls, 0u);
        EXPECT_EQ(script.output, payload.substr(0, 11));
    });
}

namespace
{

struct ConnectionLifetimeStats
{
    int opened = 0;
    int closed = 0;
    int handled = 0;
    int borrowers = 0;
    bool refuse = false;
    bool upgrade = false;
    bool spawnChild = false;
    bool upgraded = false;
    bool hooks = true;
    bool childCompleted = false;
    bool childFinishesData = false;
    coop::KillCause closeCause = coop::KillCause::None;
    sockaddr_storage peer{};
    socklen_t peerLength = 0;
    coop::Context* serving = nullptr;
    coop::Coordinator releaseChild;
    coop::Signal closedSignal{coop::Self()};
};

struct ApplicationConnectionState
{
    ConnectionLifetimeStats* stats;
    coop::Context* owner;
};

bool OpenApplicationConnection(coop::Context* ctx, const sockaddr* peer, socklen_t peerLength,
                               void* userData, void*& state)
{
    auto* stats = static_cast<ConnectionLifetimeStats*>(userData);
    ++stats->opened;
    stats->serving = ctx;
    EXPECT_NE(peer, nullptr);
    EXPECT_EQ(peerLength, sizeof(sockaddr_in));
    if (peer && peerLength == sizeof(sockaddr_in))
    {
        memcpy(&stats->peer, peer, peerLength);
        stats->peerLength = peerLength;
        EXPECT_EQ(peer->sa_family, AF_INET);
        const auto* address = reinterpret_cast<const sockaddr_in*>(peer);
        EXPECT_EQ(address->sin_addr.s_addr, htonl(INADDR_LOOPBACK));
        EXPECT_NE(address->sin_port, 0);
    }
    state = new ApplicationConnectionState{stats, ctx};
    return !stats->refuse;
}

void CloseApplicationConnection(coop::Context* ctx, void* userData, void* state)
{
    auto* application = static_cast<ApplicationConnectionState*>(state);
    auto* stats = static_cast<ConnectionLifetimeStats*>(userData);
    EXPECT_EQ(application->owner, ctx);
    EXPECT_EQ(coop::Self(), ctx);
    EXPECT_EQ(application->stats, stats);
    EXPECT_EQ(stats->borrowers, 0);
    stats->closeCause = ctx->WhyKilled();
    ++stats->closed;
    stats->closedSignal.Notify(ctx, false);
    delete application;
}

void ApplicationStateHandler(coop::http::ConnectionBase& conn, void* userData)
{
    auto* application = static_cast<ApplicationConnectionState*>(conn.GetConnectionState());
    ASSERT_NE(application, nullptr);
    auto* stats = static_cast<ConnectionLifetimeStats*>(userData);
    EXPECT_EQ(application->stats, stats);
    EXPECT_EQ(application->owner, coop::Self());
    ++stats->handled;
    if (stats->spawnChild)
    {
        ASSERT_TRUE(coop::GetCooperator()->Spawn([application](coop::Context* child)
        {
            auto* stats = application->stats;
            ASSERT_TRUE(child->GetCooperator()->Spawn([application](coop::Context* grandchild)
            {
                auto* stats = application->stats;
                ++stats->borrowers;
                // Cleanup deliberately ignores cancellation: close must join its retirement.
                auto result = coop::CoordinateWith(grandchild, &stats->releaseChild,
                                                   std::chrono::seconds(2));
                EXPECT_FALSE(result.Error());
                EXPECT_FALSE(result.TimedOut());
                EXPECT_EQ(application->stats, stats);
                --stats->borrowers;
                if (!result.Error() && !result.TimedOut())
                    stats->releaseChild.Release(grandchild, false);
            }));
        }));
    }
    if (stats->upgrade)
    {
        ASSERT_TRUE(coop::ws::Upgrade(conn));
        stats->upgraded = true;
        auto result = coop::CoordinateWithKill(coop::Self(), &stats->releaseChild,
                                               std::chrono::seconds(2));
        EXPECT_EQ(result, &stats->releaseChild);
        EXPECT_EQ(stats->closed, 0);
        if (result == &stats->releaseChild)
            stats->releaseChild.Release(coop::Self(), false);
        return;
    }
    conn.Send(200, "text/plain", "OK");
}

void AttachedResponseHandler(coop::http::ConnectionBase& conn, void* userData)
{
    auto* stats = static_cast<ConnectionLifetimeStats*>(userData);
    EXPECT_EQ(conn.GetConnectionState(), nullptr);
    ASSERT_TRUE(conn.BeginChunked(200, "text/plain"));
    if (!stats->childFinishesData) ASSERT_TRUE(conn.SendChunk("joined", 6));
    int descriptor = conn.GetDescriptor().m_fd;
    ASSERT_TRUE(coop::Spawn([&conn, stats, descriptor](coop::Context* child)
    {
        coop::Coordinator idle(child);
        auto result = coop::CoordinateWithKill(child, &idle, std::chrono::seconds(1));
        EXPECT_TRUE(result.Killed());
        EXPECT_EQ(conn.GetDescriptor().m_fd, descriptor);
        stats->childCompleted = stats->childFinishesData
            ? conn.EndChunked("joined", 6) : conn.EndChunked();
    }));
}

struct ApplicationStateServer
{
    coop::Context* owner;
    ConnectionLifetimeStats& stats;
    coop::Context::Handle handle;
    coop::http::ServerHandle control;
    coop::io::ssl::Context* tls;
    int port = 0;
    bool returned = false;

    ApplicationStateServer(coop::Context* ctx, ConnectionLifetimeStats& value,
                           coop::io::ssl::Context* tlsContext = nullptr)
        : owner(ctx), stats(value), tls(tlsContext)
    {
        EXPECT_TRUE(ctx->GetCooperator()->Spawn(
            {.priority = 0, .stackSize = 65536}, [this](coop::Context* server)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.timeout = std::chrono::milliseconds(500);
            config.handler = stats.hooks ? ApplicationStateHandler : AttachedResponseHandler;
            config.userData = &stats;
            if (stats.hooks)
            {
                config.connectionOpen = OpenApplicationConnection;
                config.connectionClose = CloseApplicationConnection;
            }
            if (tls) coop::http::RunTlsServer(server, config, *tls);
            else coop::http::RunServer(server, config);
            returned = true;
        }, &handle));
        port = control.Port();
        EXPECT_NE(port, 0);
    }

    ~ApplicationStateServer()
    {
        if (control.HasListener())
            EXPECT_TRUE(control.Drain(owner, std::chrono::milliseconds(500)));
        handle.Kill();
        owner->JoinChildren();
        EXPECT_TRUE(returned);
        EXPECT_EQ(stats.closed, stats.opened);
    }

    int Connect()
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        EXPECT_GE(fd, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        coop::io::Descriptor descriptor(coop::io::borrowed, fd);
        EXPECT_EQ(coop::io::ConnectKill(descriptor, reinterpret_cast<sockaddr*>(&address),
                                       sizeof(address), std::chrono::seconds(1)), 0);
        return fd;
    }
};

} // namespace

TEST(HttpTest, ConnectionStateSpansKeepAliveAndClosesOnEof)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ConnectionLifetimeStats stats;
        ApplicationStateServer server(ctx, stats);
        {
            coop::io::Descriptor client(server.Connect());
            for (int request = 0; request != 2; ++request)
            {
                SendString(client, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
                EXPECT_NE(RecvAll(client).find("200 OK"), std::string::npos);
                EXPECT_EQ(stats.opened, 1);
                EXPECT_EQ(stats.closed, 0);
            }
            EXPECT_EQ(stats.handled, 2);
            sockaddr_in local{};
            socklen_t size = sizeof(local);
            ASSERT_EQ(getsockname(client.m_fd, reinterpret_cast<sockaddr*>(&local), &size), 0);
            EXPECT_EQ(reinterpret_cast<const sockaddr_in*>(&stats.peer)->sin_port, local.sin_port);
        }
        auto closed = coop::CoordinateWithKill(ctx, &stats.closedSignal, std::chrono::seconds(2));
        EXPECT_FALSE(closed.TimedOut());
        EXPECT_FALSE(closed.Killed());
        EXPECT_EQ(stats.closed, 1);
        EXPECT_EQ(stats.closeCause, coop::KillCause::None);
    });
}

TEST(HttpTest, ConnectionStateRefusalClosesOnlyNewConnection)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ConnectionLifetimeStats stats;
        stats.refuse = true;
        ApplicationStateServer server(ctx, stats);
        {
            coop::io::Descriptor client(server.Connect());
            char byte;
            EXPECT_EQ(coop::io::Recv(client, &byte, 1, 0, std::chrono::seconds(1)), 0);
        }
        EXPECT_EQ(stats.opened, 1);
        EXPECT_EQ(stats.closed, 1);
        EXPECT_EQ(stats.handled, 0);
        stats.refuse = false;
        {
            coop::io::Descriptor client(server.Connect());
            SendString(client, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
            EXPECT_NE(RecvAll(client).find("200 OK"), std::string::npos);
        }
        EXPECT_EQ(stats.opened, 2);
        EXPECT_EQ(stats.closed, 2);
        EXPECT_EQ(stats.handled, 1);
        EXPECT_EQ(stats.closeCause, coop::KillCause::None);
    });
}

TEST(HttpTest, ConnectionStateKillJoinsAttachedDescendantsBeforeClose)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ConnectionLifetimeStats stats;
        stats.spawnChild = true;
        ASSERT_TRUE(stats.releaseChild.TryAcquire(ctx));
        ApplicationStateServer server(ctx, stats);
        coop::io::Descriptor client(server.Connect());
        SendString(client, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        EXPECT_NE(RecvAll(client).find("200 OK"), std::string::npos);
        EXPECT_EQ(stats.borrowers, 1);
        // Drain-mode connections are detached from the listener; kill the serving context.
        ASSERT_NE(stats.serving, nullptr);
        stats.serving->KillSelf(coop::KillCause::Kill);
        for (int i = 0; i < 30; ++i) ctx->Yield(true);
        EXPECT_EQ(stats.closed, 0);
        stats.releaseChild.Release(ctx, false);
        auto closed = coop::CoordinateWithKill(ctx, &stats.closedSignal, std::chrono::seconds(2));
        EXPECT_FALSE(closed.TimedOut());
        EXPECT_FALSE(closed.Killed());
        EXPECT_EQ(stats.borrowers, 0);
        EXPECT_EQ(stats.closed, 1);
        EXPECT_EQ(stats.closeCause, coop::KillCause::Kill);
    });
}

TEST(HttpTest, ConnectionStateRemainsOpenDuringUpgrade)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ConnectionLifetimeStats stats;
        stats.upgrade = true;
        ASSERT_TRUE(stats.releaseChild.TryAcquire(ctx));
        ApplicationStateServer server(ctx, stats);
        coop::io::Descriptor client(server.Connect());
        SendString(client,
            "GET / HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n");
        EXPECT_NE(RecvAll(client).find("101 Switching Protocols"), std::string::npos);
        EXPECT_TRUE(stats.upgraded);
        EXPECT_EQ(stats.closed, 0);
        stats.releaseChild.Release(ctx, false);
        auto closed = coop::CoordinateWithKill(ctx, &stats.closedSignal, std::chrono::seconds(2));
        EXPECT_FALSE(closed.TimedOut());
        EXPECT_FALSE(closed.Killed());
        EXPECT_EQ(stats.closed, 1);
        EXPECT_EQ(stats.closeCause, coop::KillCause::None);
    });
}


TEST(HttpTest, AttachedResponseChildFinishesBeforeUnhookedConnectionTeardown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ConnectionLifetimeStats stats;
        stats.hooks = false;
        ApplicationStateServer server(ctx, stats);
        coop::io::Descriptor client(server.Connect());
        SendString(client, "GET / HTTP/1.0\r\nHost: t\r\n\r\n");
        std::string response = RecvAll(client);
        EXPECT_NE(response.find("HTTP/1.0 200 OK\r\n"), std::string::npos);
        EXPECT_NE(response.find("joined"), std::string::npos);
        EXPECT_TRUE(stats.childCompleted);
        EXPECT_EQ(stats.opened, 0);
        EXPECT_EQ(stats.closed, 0);
    });
}

TEST(HttpTest, AttachedTlsResponseChildFinishesBeforeOrderlyConnectionTeardown)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(3));
        auto certificates = std::make_unique<test::TlsPair>();
        ConnectionLifetimeStats stats;
        stats.hooks = false;
        stats.childFinishesData = true;
        ApplicationStateServer server(ctx, stats, &certificates->serverContext);
        coop::io::Descriptor client(server.Connect());
        char staging[16384];
        coop::io::ssl::Connection tls(certificates->clientContext, client,
                                       staging, sizeof(staging));
        ASSERT_EQ(tls.HandshakeKill(), 0);
        constexpr char request[] = "GET / HTTP/1.0\r\nHost: t\r\n\r\n";
        ASSERT_EQ(coop::io::ssl::SendAllKill(tls, request, sizeof(request) - 1),
                  sizeof(request) - 1);
        std::string response;
        char buffer[512];
        int received;
        while ((received = coop::io::ssl::RecvKill(tls, buffer, sizeof(buffer),
                                                   std::chrono::seconds(1))) > 0)
            response.append(buffer, static_cast<size_t>(received));
        EXPECT_EQ(received, 0);
        EXPECT_NE(SSL_get_shutdown(tls.m_ssl) & SSL_RECEIVED_SHUTDOWN, 0);
        EXPECT_NE(response.find("HTTP/1.0 200 OK\r\n"), std::string::npos);
        EXPECT_NE(response.find("\r\n\r\njoined"), std::string::npos);
        EXPECT_TRUE(stats.childCompleted);
        EXPECT_EQ(stats.opened, 0);
        EXPECT_EQ(stats.closed, 0);
    }, {.stackSize = 65536});
}

TEST(HttpTest, ConnectionStateAccessorHasNullDefault)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        coop::io::Descriptor descriptor(coop::io::borrowed, -1);
        coop::http::PlaintextTransport transport(descriptor);
        auto conn = ctx->Allocate<HttpConn>(HTTP_EXTRA, transport, ctx, ctx->GetCooperator());
        EXPECT_EQ(conn->GetConnectionState(), nullptr);
        EXPECT_EQ(conn->coop::http::ConnectionBase::GetConnectionState(), nullptr);
    });
}

TEST(HttpTest, TlsConnectionStateSpansRequestsAndClosesHealthy)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(3));
        auto certificate = std::make_unique<test::SendPair>(test::SendTransport::MemoryTls);
        ConnectionLifetimeStats stats;
        ApplicationStateServer server(ctx, stats, &certificate->serverContext);
        {
            coop::io::Descriptor client(server.Connect());
            coop::io::ssl::Connection tls(certificate->clientContext, client,
                                           coop::io::ssl::SocketBio{});
            ASSERT_EQ(tls.HandshakeKill(), 0);
            for (int request = 0; request != 2; ++request)
            {
                const char* text = request == 0
                    ? "GET / HTTP/1.1\r\nHost: t\r\n\r\n"
                    : "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
                ASSERT_EQ(coop::io::ssl::SendAllKill(tls, text, strlen(text)),
                          static_cast<int>(strlen(text)));
                std::string response;
                char buffer[512];
                int n;
                while ((n = coop::io::ssl::RecvKill(tls, buffer, sizeof(buffer),
                                                    std::chrono::milliseconds(100))) > 0)
                    response.append(buffer, static_cast<size_t>(n));
                EXPECT_NE(response.find("200 OK\r\n"), std::string::npos);
                EXPECT_EQ(stats.opened, 1);
                EXPECT_EQ(stats.handled, request + 1);
                if (request == 0) EXPECT_EQ(stats.closed, 0);
            }
            sockaddr_in local{};
            socklen_t size = sizeof(local);
            ASSERT_EQ(getsockname(client.m_fd, reinterpret_cast<sockaddr*>(&local), &size), 0);
            EXPECT_EQ(reinterpret_cast<const sockaddr_in*>(&stats.peer)->sin_port, local.sin_port);
        }
        auto closed = coop::CoordinateWithKill(ctx, &stats.closedSignal, std::chrono::seconds(2));
        EXPECT_EQ(closed.Killed(), false);
        EXPECT_EQ(closed.TimedOut(), false);
        EXPECT_EQ(stats.closed, 1);
        EXPECT_EQ(stats.closeCause, coop::KillCause::None);
    }, {.stackSize = 65536});
}

TEST(HttpTest, TlsConnectionStateClosesAfterHandshakeFailureWithoutHandler)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        auto certificate = std::make_unique<test::SendPair>(test::SendTransport::MemoryTls);
        ConnectionLifetimeStats stats;
        ApplicationStateServer server(ctx, stats, &certificate->serverContext);
        coop::io::Descriptor client(server.Connect());
        constexpr char invalid[] = "invalid TLS handshake\n";
        ASSERT_EQ(coop::io::SendKill(client, invalid, sizeof(invalid) - 1, MSG_NOSIGNAL,
                                    std::chrono::seconds(1)), sizeof(invalid) - 1);
        ASSERT_EQ(coop::io::Shutdown(client, SHUT_WR), 0);
        auto closed = coop::CoordinateWithKill(ctx, &stats.closedSignal, std::chrono::seconds(2));
        EXPECT_FALSE(closed.Killed());
        EXPECT_FALSE(closed.TimedOut());
        EXPECT_EQ(stats.opened, 1);
        EXPECT_EQ(stats.closed, 1);
        EXPECT_EQ(stats.handled, 0);
        EXPECT_EQ(stats.closeCause, coop::KillCause::None);
    }, {.stackSize = 65536});
}

namespace
{
struct AdmissionStats
{
    size_t admissions = 0;
    size_t dispatches = 0;
    size_t userRoutes = 0;
    size_t builtins = 0;
};

void ExerciseRequestAdmission(coop::Context* ctx, bool encrypted, bool pbuf)
{
    ctx->SetDeadlineIn(std::chrono::seconds(10));
    AdmissionStats stats;
    coop::http::ServerHandle control;
    std::unique_ptr<test::TlsPair> certificates;
    if (encrypted) certificates = std::make_unique<test::TlsPair>();
    coop::Context::Handle listener;
    ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* server)
    {
        coop::http::ServerConfiguration config;
        config.port = 0;
        config.control = &control;
        config.pbufRecv = pbuf;
        config.timeout = std::chrono::seconds(1);
        config.maxRequestHeadBytes = 1024;
        config.userData = &stats;
        config.requestAdmission = [](coop::http::ConnectionBase& conn,
                                     const coop::http::RequestHead& head, void* data)
        {
            ++static_cast<AdmissionStats*>(data)->admissions;
            EXPECT_FALSE(head.method.empty());
            EXPECT_FALSE(head.target.empty());
            EXPECT_EQ(conn.InspectRequestHead(), &head);
            auto* host = head.FindUniqueHeader("Host");
            auto* origin = head.FindUniqueHeader("Origin");
            if (host && host->value == "invalid.test") return 200;
            if (!host || host->value != "allowed.test" || !origin ||
                origin->value != "https://allowed.test") return 403;
            return 0;
        };
        config.handler = [](coop::http::ConnectionBase& conn, void* data)
        {
            auto& stats = *static_cast<AdmissionStats*>(data);
            ++stats.dispatches;
            // Admission releases its snapshot before dispatch; ordinary parsing replays
            // the same request and owns no retained admission buffers after headers.
            EXPECT_EQ(conn.InspectRequestHead(), nullptr);
            const auto* request = conn.GetRequestLine();
            ASSERT_NE(request, nullptr);
            if (coop::http::StatusDispatch(conn, request->path)) { ++stats.builtins; return; }
            ++stats.userRoutes;
            ASSERT_STREQ(conn.NextArgName(), "q");
            auto* argument = conn.ReadArgValue();
            ASSERT_NE(argument, nullptr);
            const std::string value(static_cast<const char*>(argument->data), argument->size);
            ASSERT_TRUE(argument->complete);
            ASSERT_STREQ(conn.NextHeaderName(), "Host");
            auto* host = conn.ReadHeaderValue();
            ASSERT_NE(host, nullptr);
            EXPECT_EQ(std::string_view(static_cast<const char*>(host->data), host->size),
                      "allowed.test");
            std::string body;
            while (auto part = conn.NextBody())
                body.append(static_cast<const char*>(part->data), part->size);
            EXPECT_TRUE(conn.Complete());
            EXPECT_EQ(body, value == "one" ? "hello" : "");
            EXPECT_TRUE(conn.Send(200, "text/plain", std::string_view(value)));
        };
        if (encrypted)
            EXPECT_TRUE(coop::http::RunTlsServer(server, config, certificates->serverContext));
        else
            EXPECT_TRUE(coop::http::RunServer(server, config));
    }, &listener));
    ASSERT_GT(control.Port(), 0);

    auto exchange = [&](const std::string& request, std::span<const int> statuses,
                        bool sequential = false)
    {
        coop::io::Descriptor socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
        ASSERT_GE(socket.m_fd, 0);
        ASSERT_EQ(ConnectLoopbackKill(socket, control.Port()), 0);
        std::vector<char> tlsBuffer;
        std::unique_ptr<coop::io::ssl::Connection> tls;
        if (encrypted)
        {
            tlsBuffer.resize(16384);
            tls = std::make_unique<coop::io::ssl::Connection>(certificates->clientContext,
                    socket, tlsBuffer.data(), tlsBuffer.size());
            ASSERT_EQ(tls->HandshakeKill(), 0);
        }
        auto run = [&](auto transport)
        {
            using Client = coop::http::ClientConnection<decltype(transport)>;
            auto response = ctx->Allocate<Client>(Client::ExtraBytes(), transport,
                                                  "allowed.test", 2048, 512,
                                                  std::chrono::seconds(1));
            auto send = [&](std::string_view bytes)
            {
                if (tls) return coop::io::ssl::SendAllKill(*tls, bytes.data(), bytes.size());
                size_t sent = 0;
                while (sent < bytes.size())
                {
                    int n = coop::io::SendKill(socket, bytes.data() + sent, bytes.size() - sent,
                                               MSG_NOSIGNAL, std::chrono::seconds(1));
                    if (n <= 0) return n;
                    sent += static_cast<size_t>(n);
                }
                return static_cast<int>(sent);
            };
            ASSERT_EQ(send(request), static_cast<int>(request.size()));
            for (size_t i = 0; i < statuses.size(); ++i)
            {
                auto* line = response->GetResponseLine();
                ASSERT_NE(line, nullptr);
                EXPECT_EQ(line->status, statuses[i]);
                std::string body;
                while (auto part = response->NextBody())
                    body.append(static_cast<const char*>(part->data), part->size);
                EXPECT_TRUE(response->Complete());
                if (sequential && i == 0)
                {
                    ASSERT_TRUE(response->Reset());
                    const std::string second =
                        "GET /echo?q=two HTTP/1.1\r\nHost: \tallowed.test \t\r\n"
                        "Origin: \thttps://allowed.test\t \r\nConnection: close\r\n\r\n";
                    ASSERT_EQ(send(second), static_cast<int>(second.size()));
                }
                else if (i + 1 < statuses.size()) ASSERT_TRUE(response->Reset());
            }
        };
        if (tls) run(coop::http::TlsTransport(*tls, socket));
        else run(coop::http::PlaintextTransport(socket));
    };

    const int refused[] = {403};
    exchange("POST /api/perf/enable HTTP/1.1\r\nHost: refused.test\r\n"
             "Origin: \thttps://allowed.test\t \r\nContent-Length: 4\r\n\r\nbody", refused);
    exchange("GET /echo?q=two HTTP/1.1\r\nHost: \tallowed.test \t\r\n"
             "Origin: https://refused.test\r\n\r\n", refused);
    const int malformedHost[] = {400};
    exchange("GET /echo HTTP/1.1\r\nOrigin: \thttps://allowed.test\t \r\n\r\n", malformedHost);
    exchange("GET /echo HTTP/1.1\r\nHost: \tallowed.test \t\r\nHost: refused.test\r\n"
             "Origin: \thttps://allowed.test\t \r\n\r\n", malformedHost);
    const int invalidStatus[] = {500};
    exchange("GET /echo?q=two HTTP/1.1\r\nHost: invalid.test\r\n\r\n", invalidStatus);
    EXPECT_EQ(stats.admissions, 3);
    EXPECT_EQ(stats.dispatches, 0);
    const int oversized[] = {431};
    exchange("GET /echo HTTP/1.1\r\nX-Large: " + std::string(1500, 'v') + "\r\n\r\n", oversized);
    EXPECT_EQ(stats.admissions, 3);
    EXPECT_EQ(stats.dispatches, 0);

    const std::string first =
        "POST /echo?q=one HTTP/1.1\r\nHost: \tallowed.test \t\r\n"
        "Origin: \thttps://allowed.test\t \r\nContent-Length: 5\r\n\r\nhello";
    const std::string second =
        "GET /echo?q=two HTTP/1.1\r\nHost: \tallowed.test \t\r\n"
        "Origin: \thttps://allowed.test\t \r\nConnection: close\r\n\r\n";
    const int accepted[] = {200, 200};
    exchange(first + second, accepted);
    exchange(first, accepted, true);
    const int builtin[] = {200};
    exchange("GET /api/sampler HTTP/1.1\r\nHost: \tallowed.test \t\r\n"
             "Origin: \thttps://allowed.test\t \r\nConnection: close\r\n\r\n", builtin);
    EXPECT_EQ(stats.admissions, 8);
    EXPECT_EQ(stats.dispatches, 5);
    EXPECT_EQ(stats.userRoutes, 4);
    EXPECT_EQ(stats.builtins, 1);
    EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
    listener.Kill();
    ctx->JoinChildren();
}
} // namespace

TEST(HttpTest, RequestAdmissionPrecedesRoutesAndPreservesKeepAliveAndPipeline)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ExerciseRequestAdmission(ctx, false, false);
    }, {.stackSize = 131072});
}

TEST(HttpTest, TlsRequestAdmissionPrecedesRoutesAndPreservesKeepAliveAndPipeline)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        ExerciseRequestAdmission(ctx, true, false);
    }, {.stackSize = 131072});
}

TEST(HttpPbufTest, RequestAdmissionPreservesBodyAndPipelineAcrossWindows)
{
    RunWithBufferRing(64, 128, [](coop::Context* ctx)
    {
        ExerciseRequestAdmission(ctx, false, true);
    });
}

TEST(HttpTest, NoBodyEndpointsRejectMalformedChunkBeforeHandlerAndClose)
{
    coop::http::ServerHandle control;
    bool handlerRan = false;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::Context::Handle serverHandle;
        ctx->GetCooperator()->Spawn({.priority = 0, .stackSize = 65536},
            [&](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.timeout = std::chrono::milliseconds(500);
            config.handler = [](coop::http::ConnectionBase& conn, void* data)
            {
                auto path = conn.GetRequestLine()->path;
                if (coop::http::StatusDispatch(conn, path)) return;
                if (!conn.RequireNoBody()) return;
                *static_cast<bool*>(data) = true;
                conn.Send(200, "text/plain", "ok");
            };
            config.userData = &handlerRan;
            coop::http::RunServer(serverCtx, config);
        }, &serverHandle);
        const auto port = control.Port();
        ASSERT_NE(port, 0);

        for (const char* path : {"/no-body", "/api/sampler"})
        {
            SCOPED_TRACE(path);
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            ASSERT_GE(fd, 0);
            coop::io::Descriptor client(fd, coop::GetUring());
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            ASSERT_EQ(coop::io::ConnectKill(client,
                reinterpret_cast<sockaddr*>(&address), sizeof(address),
                std::chrono::milliseconds(500)), 0);
            std::string request = "GET " + std::string(path) +
                " HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\ninvalid chunk\r\n";
            ASSERT_EQ(coop::io::SendKill(client, request.data(), request.size(), MSG_NOSIGNAL,
                std::chrono::milliseconds(500)), static_cast<int>(request.size()));
            ASSERT_EQ(::shutdown(client.m_fd, SHUT_WR), 0);
            std::string response;
            char buffer[512];
            int received;
            while ((received = coop::io::RecvKill(client, buffer, sizeof(buffer), 0,
                        std::chrono::milliseconds(500))) > 0)
                response.append(buffer, static_cast<size_t>(received));
            EXPECT_EQ(received, 0);
            EXPECT_NE(response.find("400 Bad Request\r\n"), std::string::npos);
            EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
            EXPECT_EQ(response.find("200 OK\r\n"), std::string::npos);
            EXPECT_FALSE(handlerRan);
        }
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        serverHandle.Kill();
    });
}

TEST(HttpTest, RefusedLargeBodyDeliversResponseBeforeOrderlyClose)
{
    coop::http::ServerHandle control;
    bool handlerRan = false;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(5));
        coop::Context::Handle listener;
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* server)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.handler = [](coop::http::ConnectionBase& conn, void* value)
            {
                if (!conn.RequireNoBody()) return;
                *static_cast<bool*>(value) = true;
                conn.Send(200, "text/plain", "unexpected");
            };
            config.userData = &handlerRan;
            EXPECT_TRUE(coop::http::RunServer(server, config));
        }, &listener));
        ASSERT_NE(control.Port(), 0);

        for (const char* framing : {"Content-Length: 524288\r\n",
                                   "Transfer-Encoding: chunked\r\n",
                                   "Content-Length: invalid\r\n"})
        {
            SCOPED_TRACE(framing);
            coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
            ASSERT_GE(client.m_fd, 0);
            int size = 16384;
            ASSERT_EQ(setsockopt(client.m_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(control.Port());
            ASSERT_EQ(coop::io::ConnectKill(client,
                reinterpret_cast<sockaddr*>(&address), sizeof(address),
                std::chrono::seconds(1)), 0);
            std::string upload = "POST /no-body HTTP/1.1\r\nHost: example.test\r\n" + std::string(framing) +
                                 "\r\n" + std::string(524288, 'x');
            coop::CountdownLatch uploaded(1);
            bool bodySent = false;
            ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context*)
            {
                size_t at = 0;
                while (at < upload.size())
                {
                    int sent = coop::io::SendFastpathKill(client, upload.data() + at,
                        upload.size() - at, MSG_NOSIGNAL, std::chrono::seconds(1));
                    if (sent <= 0) break;
                    at += static_cast<size_t>(sent);
                }
                bodySent = at == upload.size();
                ::shutdown(client.m_fd, SHUT_WR);
                uploaded.Arrive();
            }));
            std::string response;
            char buffer[512];
            int received;
            while ((received = coop::io::RecvKill(client, buffer, sizeof(buffer), 0,
                                                  std::chrono::seconds(1))) > 0)
                response.append(buffer, static_cast<size_t>(received));
            EXPECT_EQ(received, 0);
            EXPECT_NE(response.find("400 Bad Request\r\n"), std::string::npos);
            EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
            EXPECT_EQ(uploaded.WaitKill(ctx), coop::CountdownLatch::WaitStatus::Completed);
            EXPECT_TRUE(bodySent);
            EXPECT_FALSE(handlerRan);
        }
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        listener.Kill();
    }, {.stackSize = 65536});
}

TEST(HttpTest, RefusedBodyOverDrainCapPreservesResponseWithoutReset)
{
    coop::http::ServerHandle control;
    coop::CountdownLatch entered(1);
    test::RunInCooperator([&](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(5));
        coop::Context::Handle listener;
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* server)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.userData = &entered;
            config.handler = [](coop::http::ConnectionBase& conn, void* data)
            {
                static_cast<coop::CountdownLatch*>(data)->Arrive();
                EXPECT_FALSE(conn.RequireNoBody());
            };
            EXPECT_TRUE(coop::http::RunServer(server, config));
        }, &listener));
        ASSERT_NE(control.Port(), 0);
        coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
        ASSERT_GE(client.m_fd, 0);
        ASSERT_EQ(ConnectLoopbackKill(client, control.Port()), 0);
        const std::string request = "POST / HTTP/1.1\r\nContent-Length: 2097152\r\nHost: example.test\r\n\r\n" +
                                    std::string(2 * 1024 * 1024, 'x');
        // Queue the whole upload without yielding to the serving context. The body is
        // twice the refusal drain cap, and remains in flight when that cap is reached.
        //
        ASSERT_EQ(::send(client.m_fd, request.data(), request.size(), MSG_NOSIGNAL | MSG_DONTWAIT),
                  static_cast<ssize_t>(request.size()));

        ASSERT_EQ(entered.WaitKill(ctx), coop::CountdownLatch::WaitStatus::Completed);
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        // POLLERR observes a reset that may follow the already queued FIN.
        int ready = coop::io::PollKill(client, POLLERR, std::chrono::milliseconds(50));
        EXPECT_TRUE(ready == -ETIMEDOUT || (ready >= 0 && (ready & POLLERR) == 0));
        int error = 0;
        socklen_t length = sizeof(error);
        ASSERT_EQ(::getsockopt(client.m_fd, SOL_SOCKET, SO_ERROR, &error, &length), 0);
        EXPECT_EQ(error, 0);
        std::string response;
        char buffer[512];
        int received;
        while ((received = coop::io::RecvKill(client, buffer, sizeof(buffer), 0,
                                              std::chrono::seconds(1))) > 0)
            response.append(buffer, static_cast<size_t>(received));
        EXPECT_EQ(received, 0);
        EXPECT_NE(response.find("400 Bad Request\r\n"), std::string::npos);
        listener.Kill();
    }, {.stackSize = 65536});
}

TEST(HttpTest, RefusalDrainIsBoundedForAnIdleUploader)
{
    coop::http::ServerHandle control;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(5));
        coop::Context::Handle listener;
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* server)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.handler = [](coop::http::ConnectionBase& conn, void*)
            {
                EXPECT_FALSE(conn.RequireNoBody());
            };
            EXPECT_TRUE(coop::http::RunServer(server, config));
        }, &listener));
        ASSERT_NE(control.Port(), 0);
        coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
        ASSERT_GE(client.m_fd, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(control.Port());
        ASSERT_EQ(coop::io::ConnectKill(client, reinterpret_cast<sockaddr*>(&address),
                                      sizeof(address), std::chrono::seconds(1)), 0);
        const char request[] = "POST / HTTP/1.1\r\nContent-Length: 100000000\r\nHost: example.test\r\n\r\n";
        ASSERT_EQ(coop::io::SendKill(client, request, sizeof(request) - 1, MSG_NOSIGNAL,
                                    std::chrono::seconds(1)), sizeof(request) - 1);
        std::string response;
        char buffer[512];
        int received;
        while ((received = coop::io::RecvKill(client, buffer, sizeof(buffer), 0,
                                              std::chrono::seconds(1))) > 0)
            response.append(buffer, static_cast<size_t>(received));
        EXPECT_EQ(received, 0);
        EXPECT_NE(response.find("400 Bad Request\r\n"), std::string::npos);
        // The peer keeps its write side open. A missing drain cap makes the soft drain
        // expire and report false, rather than leaving this test blocked indefinitely.
        //
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        listener.Kill();
    }, {.stackSize = 65536});
}

TEST(HttpTest, Http10TlsCompletionTruncationErrorsAndFixedKeepAlive)
{
    coop::http::ServerHandle control;
    coop::io::ssl::Context serverContext{coop::io::ssl::Mode::Server};
    test::RunInCooperator([&](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(10));
        auto certificates = std::make_unique<test::TlsPair>();
        ASSERT_EQ(SSL_CTX_use_certificate(serverContext.m_ctx,
            SSL_CTX_get0_certificate(certificates->serverContext.m_ctx)), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey(serverContext.m_ctx,
            SSL_CTX_get0_privatekey(certificates->serverContext.m_ctx)), 1);
        coop::Context::Handle listener;
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.timeout = std::chrono::milliseconds(500);
            config.handler = [](coop::http::ConnectionBase& conn, void*)
            {
                auto path = conn.GetRequestLine()->path;
                const bool fixed = path == "/fixed";
                const bool bad = path == "/bad";
                const bool abandoned = path == "/abandoned";
                const bool failed = path == "/failed";
                const bool killed = path == "/killed";
                const bool completeKilled = path == "/complete-killed";
                const bool completeExpired = path == "/complete-expired";
                conn.SkipHeaders();
                if (bad) return;
                if (fixed) { EXPECT_TRUE(conn.Send(200, "text/plain", "hello")); return; }
                EXPECT_TRUE(conn.BeginChunked(200, "text/plain"));
                EXPECT_TRUE(conn.SendChunk("hel", 3));
                if (abandoned) return;
                if (failed) { EXPECT_FALSE(conn.AppendHeader("Trailer", "X")); return; }
                if (killed) { coop::Self()->KillSelf(coop::KillCause::Kill); return; }
                EXPECT_TRUE(conn.EndChunked("lo", 2));
                if (completeKilled) coop::Self()->KillSelf(coop::KillCause::Kill);
                if (completeExpired) coop::Self()->SetDeadlineIn(std::chrono::microseconds(0));
            };
            EXPECT_TRUE(coop::http::RunTlsServer(serverCtx, config, serverContext));
        }, &listener));
        ASSERT_GT(control.Port(), 0);
        for (const char* path : {"/complete", "/complete-killed", "/complete-expired",
                                 "/abandoned", "/failed", "/killed", "/bad", "/fixed"})
        {
            SCOPED_TRACE(path);
            coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
            ASSERT_GE(client.m_fd, 0);
            ASSERT_EQ(ConnectLoopbackKill(client, control.Port()), 0);
            char staging[16384];
            coop::io::ssl::Connection tls(certificates->clientContext, client, staging, sizeof(staging));
            ASSERT_EQ(tls.HandshakeKill(), 0);
            std::string request = std::string("GET ") + path + " HTTP/1.0\r\n";
            if (std::string_view(path) == "/fixed")
                request += "Connection: keep-alive\r\n\r\n"
                           "GET /fixed HTTP/1.0\r\nConnection: close\r\n\r\n";
            else if (std::string_view(path) == "/bad")
                request += "Content-Length: invalid\r\n\r\n";
            else request += "Connection: keep-alive\r\n\r\n";
            ASSERT_EQ(coop::io::ssl::SendAllKill(tls, request.data(), request.size()),
                      static_cast<int>(request.size()));
            std::string response;
            char buffer[1024];
            int received;
            while ((received = coop::io::ssl::RecvKill(tls, buffer, sizeof(buffer),
                        std::chrono::seconds(1))) > 0)
                response.append(buffer, static_cast<size_t>(received));
            if (std::string_view(path).starts_with("/complete"))
            {
                EXPECT_EQ(received, 0);
                EXPECT_NE(SSL_get_shutdown(tls.m_ssl) & SSL_RECEIVED_SHUTDOWN, 0);
                EXPECT_NE(response.find("\r\n\r\nhello"), std::string::npos);
            }
            else
            {
                EXPECT_LT(received, 0);
                EXPECT_EQ(SSL_get_shutdown(tls.m_ssl) & SSL_RECEIVED_SHUTDOWN, 0);
            }
            if (std::string_view(path) == "/bad")
                EXPECT_EQ(response.find("HTTP/1.0 400 Bad Request\r\n"), 0);
            else EXPECT_EQ(response.find("HTTP/1.0 200 OK\r\n"), 0);
            if (std::string_view(path) == "/fixed")
            {
                EXPECT_NE(response.find("Content-Length: 5\r\n"), std::string::npos);
                EXPECT_NE(response.find("Connection: keep-alive\r\n"), std::string::npos);
                EXPECT_NE(response.find("HTTP/1.0 200 OK\r\n", 1), std::string::npos);
            }
        }
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        listener.Kill();
    }, {.stackSize = 65536});
}

TEST(HttpTest, Http10CompletedPlaintextStreamSurvivesCancellation)
{
    coop::http::ServerHandle control;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::Context::Handle listener;
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.handler = [](coop::http::ConnectionBase& conn, void*)
            {
                const auto path = conn.GetRequestLine()->path;
                const bool expired = path.ends_with("expired");
                if (!path.starts_with("/unparsed-")) conn.SkipHeaders();
                ASSERT_EQ(conn.ParseError(), 0);
                ASSERT_TRUE(conn.BeginChunked(200, "text/plain"));
                ASSERT_TRUE(conn.EndChunked("complete", 8));
                if (expired) coop::Self()->SetDeadlineIn(std::chrono::microseconds(0));
                else coop::Self()->KillSelf(coop::KillCause::Kill);
            };
            EXPECT_TRUE(coop::http::RunServer(serverCtx, config));
        }, &listener));
        ASSERT_GT(control.Port(), 0);
        for (const char* path : {"/killed", "/expired", "/unparsed-killed", "/unparsed-expired"})
        {
            SCOPED_TRACE(path);
            coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
            ASSERT_EQ(ConnectLoopbackKill(client, control.Port()), 0);
            const std::string request = std::string("GET ") + path + " HTTP/1.0\r\n\r\n";
            ASSERT_EQ(coop::io::SendKill(client, request.data(), request.size(), MSG_NOSIGNAL,
                      std::chrono::seconds(1)), request.size());
            std::string response;
            char buffer[1024];
            int received;
            while ((received = coop::io::RecvKill(client, buffer, sizeof(buffer), 0,
                        std::chrono::seconds(1))) > 0)
                response.append(buffer, static_cast<size_t>(received));
            EXPECT_EQ(received, 0);
            EXPECT_NE(response.find("\r\n\r\ncomplete"), std::string::npos);
        }
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        listener.Kill();
    });
}

TEST(HttpTest, Http10AbandonedPlaintextStreamResetsInsteadOfEndingNormally)
{
    coop::http::ServerHandle control;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        coop::Context::Handle listener;
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.handler = [](coop::http::ConnectionBase& conn, void*)
            {
                conn.SkipHeaders();
                EXPECT_TRUE(conn.BeginChunked(200, "text/plain"));
                EXPECT_TRUE(conn.SendChunk("partial", 7));
            };
            EXPECT_TRUE(coop::http::RunServer(serverCtx, config));
        }, &listener));
        ASSERT_GT(control.Port(), 0);
        coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
        ASSERT_EQ(ConnectLoopbackKill(client, control.Port()), 0);
        constexpr char request[] = "GET / HTTP/1.0\r\n\r\n";
        ASSERT_EQ(coop::io::SendKill(client, request, sizeof(request) - 1, MSG_NOSIGNAL,
                  std::chrono::seconds(1)), sizeof(request) - 1);
        char buffer[1024];
        int received;
        while ((received = coop::io::RecvKill(client, buffer, sizeof(buffer), 0,
                    std::chrono::seconds(1))) > 0) {}
        EXPECT_EQ(received, -ECONNRESET);
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        listener.Kill();
    });
}

TEST(HttpTest, TlsClosureAlertDoesNotWaitForPeerAndHonorsWriteBounds)
{
    for (bool socketBio : {false, true})
    {
        test::RunInCooperator([&](coop::Context* ctx)
        {
            ctx->SetDeadlineIn(std::chrono::seconds(3));
            auto pair = std::make_unique<test::TlsPair>(socketBio);
            EXPECT_EQ(pair->serverTls->ShutdownWriteKill(std::chrono::milliseconds(500)), 0);
            char byte;
            EXPECT_EQ(pair->Recv(&byte, 1), 0);
            EXPECT_NE(SSL_get_shutdown(pair->clientTls->m_ssl) & SSL_RECEIVED_SHUTDOWN, 0);

            auto stalled = std::make_unique<test::TlsPair>(socketBio);
            stalled->FillSocket();
            EXPECT_EQ(stalled->serverTls->ShutdownWriteKill(std::chrono::milliseconds(30)),
                      -ETIMEDOUT);
            EXPECT_EQ(stalled->serverTls->ShutdownWriteKill(std::chrono::milliseconds(30)),
                      -EPIPE);

            auto expired = std::make_unique<test::TlsPair>(socketBio);
            ctx->SetDeadlineIn(std::chrono::microseconds(0));
            EXPECT_EQ(expired->serverTls->ShutdownWriteKill(std::chrono::milliseconds(30)), 0);
            EXPECT_EQ(ctx->WhyKilled(), coop::KillCause::Deadline);
            EXPECT_NE(SSL_get_shutdown(expired->serverTls->m_ssl) & SSL_SENT_SHUTDOWN, 0);
            EXPECT_EQ(expired->Recv(&byte, 1), 0);
        }, {.stackSize = 65536});
    }
}

TEST(HttpTest, RefusalDrainPreservesResetForAnUnfinishedHttp10Stream)
{
    coop::http::ServerHandle control;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(3));
        coop::Context::Handle listener;
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* server)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.handler = [](coop::http::ConnectionBase& conn, void*)
            {
                conn.SkipHeaders();
                EXPECT_EQ(conn.Error(), 0);
                EXPECT_TRUE(conn.BeginChunked(200, "text/plain"));
                EXPECT_TRUE(conn.SendChunk("unfinished", 10));
            };
            EXPECT_TRUE(coop::http::RunServer(server, config));
        }, &listener));
        ASSERT_NE(control.Port(), 0);
        coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
        ASSERT_GE(client.m_fd, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(control.Port());
        ASSERT_EQ(coop::io::ConnectKill(client, reinterpret_cast<sockaddr*>(&address),
                                      sizeof(address), std::chrono::seconds(1)), 0);
        const char request[] = "GET / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\ninvalid\r\n";
        ASSERT_EQ(coop::io::SendKill(client, request, sizeof(request) - 1, MSG_NOSIGNAL,
                                    std::chrono::seconds(1)), sizeof(request) - 1);
        char buffer[512];
        int received;
        while ((received = coop::io::RecvKill(client, buffer, sizeof(buffer), 0,
                                              std::chrono::seconds(1))) > 0) {}
        EXPECT_EQ(received, -ECONNRESET);
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        listener.Kill();
    });
}

TEST(HttpTest, CompletedHttp10StreamSurvivesKillDuringRequestDrain)
{
    coop::http::ServerHandle control;
    struct State
    {
        coop::CountdownLatch complete{1};
        coop::Context* serving = nullptr;
    } state;
    test::RunInCooperator([&](coop::Context* ctx)
    {
        ctx->SetDeadlineIn(std::chrono::seconds(3));
        coop::Context::Handle listener;
        ASSERT_TRUE(coop::Spawn({.stackSize = 65536}, [&](coop::Context* server)
        {
            coop::http::ServerConfiguration config;
            config.port = 0;
            config.control = &control;
            config.userData = &state;
            config.handler = [](coop::http::ConnectionBase& conn, void* data)
            {
                auto& state = *static_cast<State*>(data);
                state.serving = coop::Self();
                ASSERT_TRUE(conn.BeginChunked(200, "text/plain"));
                ASSERT_TRUE(conn.EndChunked("complete", 8));
                state.complete.Arrive();
            };
            EXPECT_TRUE(coop::http::RunServer(server, config));
        }, &listener));
        coop::io::Descriptor client(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
        ASSERT_GE(client.m_fd, 0);
        ASSERT_EQ(ConnectLoopbackKill(client, control.Port()), 0);
        constexpr char request[] = "GET / HTTP/1.0\r\n";
        ASSERT_EQ(coop::io::SendKill(client, request, sizeof(request) - 1, MSG_NOSIGNAL,
                  std::chrono::seconds(1)), sizeof(request) - 1);
        ASSERT_EQ(state.complete.WaitKill(ctx), coop::CountdownLatch::WaitStatus::Completed);
        // Arrival does not yield: the serving context has returned from its handler and
        // blocked in the request-header drain before this context resumes.
        //
        ASSERT_NE(state.serving, nullptr);
        EXPECT_FALSE(state.serving->IsKilled());
        state.serving->KillSelf(coop::KillCause::Kill);
        std::string response;
        char buffer[512];
        int received;
        while ((received = coop::io::RecvKill(client, buffer, sizeof(buffer), 0,
                                              std::chrono::seconds(1))) > 0)
            response.append(buffer, static_cast<size_t>(received));
        EXPECT_EQ(received, 0);
        EXPECT_NE(response.find("\r\n\r\ncomplete"), std::string::npos);
        EXPECT_TRUE(control.Drain(ctx, std::chrono::seconds(1)));
        listener.Kill();
    }, {.stackSize = 65536});
}
