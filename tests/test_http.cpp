#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/alloc.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/http/connection.h"
#include "coop/http/transport.h"

using HttpConn = coop::http::Connection<coop::http::PlaintextTransport>;

static constexpr size_t HTTP_EXTRA = HttpConn::ExtraBytes();

#include "test_helpers.h"

namespace
{

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
            "Content-Length: 13\r\n"
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
            "Transfer-Encoding: chunked\r\n"
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
            "X-Custom: test\r\n"
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
            "X-Extra: ignored\r\n"
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

        SendString(client, "GET / HTTP/1.1\r\n\r\n");

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

        SendString(client, "GET / HTTP/1.1\r\n\r\n");

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
