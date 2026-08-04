#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coop/alloc.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/io/descriptor.h"
#include "coop/io/recv.h"
#include "coop/io/send.h"
#include "coop/http/connection.h"
#include "coop/http/client.h"
#include "coop/http/server.h"
#include "coop/io/recv_source.h"
#include "coop/io/buffer_ring.h"
#include "coop/io/uring.h"
#include "coop/thread.h"
#include <functional>
#include "coop/http/transport.h"

using HttpConn = coop::http::Connection<coop::http::PlaintextTransport>;
using HttpClient = coop::http::ClientConnection<coop::http::PlaintextTransport>;

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

void OkHandler(coop::http::ConnectionBase& conn)
{
    conn.Send(200, "text/plain", "OK");
}

const coop::http::Route kOkRoutes[] = { { "/ok", &OkHandler } };

} // end anonymous namespace

TEST(HttpTest, RunServerMultishotAcceptRoundtrip)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Fixed port with pid salt — the test host is shared
        //
        int port = 40000 + (getpid() % 20000);

        coop::Context::Handle serverHandle;
        bool serverReturned = false;
        ctx->GetCooperator()->Spawn(
            {.priority = 0, .stackSize = 65536},
            [&, port](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = port;
            config.multishotAccept = true;
            config.maxPendingAccepts = 8;
            config.name = "TestMultishotServer";
            coop::http::RunServer(serverCtx, config, kOkRoutes, 1);
            serverReturned = true;
        }, &serverHandle);

        // Let the server bind and arm
        //
        for (int i = 0; i < 10; i++)
        {
            ctx->Yield(true);
        }

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
        fn(ctx);
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
    // Multishot accept + pbuf-chunk parsing together — the modern server data path
    //
    RunWithBufferRing(64, 2048, [](coop::Context* ctx)
    {
        int port = 41000 + (getpid() % 20000);

        coop::Context::Handle serverHandle;
        bool serverReturned = false;
        ctx->GetCooperator()->Spawn(
            {.priority = 0, .stackSize = 65536},
            [&, port](coop::Context* serverCtx)
        {
            coop::http::ServerConfiguration config;
            config.port = port;
            config.multishotAccept = true;
            config.pbufRecv = true;
            config.name = "TestModernServer";
            coop::http::RunServer(serverCtx, config, kOkRoutes, 1);
            serverReturned = true;
        }, &serverHandle);

        for (int i = 0; i < 10; i++)
        {
            ctx->Yield(true);
        }

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
    });
}
