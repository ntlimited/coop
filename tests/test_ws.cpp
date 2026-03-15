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
#include "coop/ws/types.h"
#include "coop/ws/connection.h"
#include "coop/ws/upgrade.h"
#include "coop/ws/sha1.h"

using HttpConn = coop::http::Connection<coop::http::PlaintextTransport>;
using WsConn   = coop::ws::Connection<coop::http::PlaintextTransport>;

static constexpr size_t HTTP_EXTRA = HttpConn::ExtraBytes();
static constexpr size_t WS_EXTRA   = WsConn::ExtraBytes();

#include "test_helpers.h"

namespace
{

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

void SendBytes(coop::io::Descriptor& desc, const void* data, size_t size)
{
    coop::io::SendAll(desc, data, size);
}

void SendString(coop::io::Descriptor& desc, const char* s)
{
    SendBytes(desc, s, strlen(s));
}

std::string RecvAll(coop::io::Descriptor& desc, size_t maxBytes = 8192)
{
    std::string result;
    char buf[1024];
    while (result.size() < maxBytes)
    {
        int n = coop::io::Recv(desc, buf, sizeof(buf), 0,
                                std::chrono::milliseconds(100));
        if (n <= 0) break;
        result.append(buf, static_cast<size_t>(n));
    }
    return result;
}

// Build a masked WebSocket frame (client → server).
//
std::string BuildWsFrame(coop::ws::Opcode opcode, bool fin,
                          const void* payload, size_t payloadSize,
                          uint8_t maskKey[4])
{
    std::string frame;

    uint8_t b0 = (fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode);
    frame.push_back(static_cast<char>(b0));

    // Mask bit set (client frames are always masked).
    //
    if (payloadSize <= 125)
    {
        frame.push_back(static_cast<char>(0x80 | payloadSize));
    }
    else if (payloadSize <= 65535)
    {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>(payloadSize >> 8));
        frame.push_back(static_cast<char>(payloadSize & 0xFF));
    }
    else
    {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; i--)
            frame.push_back(static_cast<char>((payloadSize >> (8 * i)) & 0xFF));
    }

    // Mask key.
    //
    frame.append(reinterpret_cast<const char*>(maskKey), 4);

    // Masked payload.
    //
    auto* src = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < payloadSize; i++)
        frame.push_back(static_cast<char>(src[i] ^ maskKey[i & 3]));

    return frame;
}

// Send the standard WebSocket upgrade request.
//
void SendUpgradeRequest(coop::io::Descriptor& client)
{
    SendString(client,
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n");
}

// Parse server's unmasked frame from raw bytes. Returns the payload.
//
struct ParsedFrame
{
    coop::ws::Opcode opcode;
    bool fin;
    std::string payload;
    bool valid{false};
};

ParsedFrame ParseServerFrame(const std::string& raw, size_t offset = 0)
{
    ParsedFrame f;
    if (raw.size() < offset + 2) return f;

    uint8_t b0 = static_cast<uint8_t>(raw[offset]);
    uint8_t b1 = static_cast<uint8_t>(raw[offset + 1]);
    f.fin = (b0 & 0x80) != 0;
    f.opcode = static_cast<coop::ws::Opcode>(b0 & 0x0F);
    bool masked = (b1 & 0x80) != 0;
    size_t len = b1 & 0x7F;
    size_t pos = offset + 2;

    if (len == 126)
    {
        if (raw.size() < pos + 2) return f;
        len = (static_cast<size_t>(static_cast<uint8_t>(raw[pos])) << 8)
            | static_cast<uint8_t>(raw[pos + 1]);
        pos += 2;
    }
    else if (len == 127)
    {
        if (raw.size() < pos + 8) return f;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | static_cast<uint8_t>(raw[pos + i]);
        pos += 8;
    }

    if (masked) pos += 4;  // server frames shouldn't be masked, but skip if present

    if (raw.size() < pos + len) return f;
    f.payload.assign(raw, pos, len);
    f.valid = true;
    return f;
}

} // end anonymous namespace

// -------------------------------------------------------------------------------------
// SHA-1 — known test vector from RFC 6455 Section 4.2.2
// -------------------------------------------------------------------------------------

TEST(WsTest, SHA1AcceptKey)
{
    // RFC 6455 example: key = "dGhlIHNhbXBsZSBub25jZQ=="
    // Expected accept: "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    //
    char accept[32];
    coop::ws::detail::ComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==", 24, accept);
    EXPECT_STREQ(accept, "fgMhHRkFb6vkp88ijC+kVh06XXU=");
}

// -------------------------------------------------------------------------------------
// Upgrade handshake
// -------------------------------------------------------------------------------------

TEST(WsTest, UpgradeSuccess)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendUpgradeRequest(client);

        coop::http::PlaintextTransport httpTransport(server);
        auto httpConn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            httpTransport, ctx, ctx->GetCooperator());

        auto* req = httpConn->GetRequestLine();
        ASSERT_NE(req, nullptr);
        EXPECT_EQ(req->path, "/ws");

        bool ok = coop::ws::Upgrade(*httpConn);
        EXPECT_TRUE(ok);

        // Client should receive the 101 response.
        //
        std::string resp = RecvAll(client);
        EXPECT_NE(resp.find("101 Switching Protocols"), std::string::npos);
        EXPECT_NE(resp.find("fgMhHRkFb6vkp88ijC+kVh06XXU="), std::string::npos);
    });
}

TEST(WsTest, UpgradeMissingHeaders)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        // Missing Upgrade header.
        //
        SendString(client,
            "GET /ws HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n");

        coop::http::PlaintextTransport httpTransport(server);
        auto httpConn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            httpTransport, ctx, ctx->GetCooperator());

        httpConn->GetRequestLine();
        bool ok = coop::ws::Upgrade(*httpConn);
        EXPECT_FALSE(ok);

        std::string resp = RecvAll(client);
        EXPECT_NE(resp.find("400"), std::string::npos);
    });
}

// -------------------------------------------------------------------------------------
// Text frame — send masked from "client", receive on server ws connection
// -------------------------------------------------------------------------------------

TEST(WsTest, TextFrame)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        // Perform upgrade.
        //
        SendUpgradeRequest(client);

        coop::http::PlaintextTransport httpTransport(server);
        auto httpConn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            httpTransport, ctx, ctx->GetCooperator());
        httpConn->GetRequestLine();
        ASSERT_TRUE(coop::ws::Upgrade(*httpConn));

        // Drain 101 response on client side.
        //
        RecvAll(client);

        // Send a masked text frame from client.
        //
        uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
        std::string payload = "hello world";
        std::string frame = BuildWsFrame(coop::ws::Opcode::Text, true,
                                          payload.data(), payload.size(), mask);
        SendBytes(client, frame.data(), frame.size());

        // Construct WS connection on server side.
        //
        coop::http::PlaintextTransport wsTransport(server);
        auto ws = ctx->Allocate<WsConn>(WS_EXTRA,
            wsTransport, ctx,
            WsConn::DEFAULT_RECV_BUFFER_SIZE,
            WsConn::DEFAULT_SEND_BUFFER_SIZE,
            std::chrono::seconds(5),
            httpConn->LeftoverData(), httpConn->LeftoverSize());

        auto* f = ws->NextFrame();
        ASSERT_NE(f, nullptr);
        EXPECT_TRUE(f->IsText());
        EXPECT_TRUE(f->fin);
        EXPECT_TRUE(f->complete);
        EXPECT_EQ(f->size, payload.size());
        EXPECT_EQ(std::string_view(static_cast<const char*>(f->data), f->size), "hello world");
    });
}

// -------------------------------------------------------------------------------------
// Server sends text frame — client verifies unmasked
// -------------------------------------------------------------------------------------

TEST(WsTest, SendText)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendUpgradeRequest(client);

        coop::http::PlaintextTransport httpTransport(server);
        auto httpConn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            httpTransport, ctx, ctx->GetCooperator());
        httpConn->GetRequestLine();
        ASSERT_TRUE(coop::ws::Upgrade(*httpConn));

        RecvAll(client);  // drain 101

        coop::http::PlaintextTransport wsTransport(server);
        auto ws = ctx->Allocate<WsConn>(WS_EXTRA,
            wsTransport, ctx,
            WsConn::DEFAULT_RECV_BUFFER_SIZE,
            WsConn::DEFAULT_SEND_BUFFER_SIZE,
            std::chrono::seconds(5),
            httpConn->LeftoverData(), httpConn->LeftoverSize());

        // Server sends a text frame.
        //
        std::string msg = "response payload";
        EXPECT_TRUE(ws->SendText(msg.data(), msg.size()));

        // Client reads the raw frame and verifies.
        //
        std::string raw = RecvAll(client);
        ASSERT_GE(raw.size(), 2u + msg.size());

        auto parsed = ParseServerFrame(raw);
        ASSERT_TRUE(parsed.valid);
        EXPECT_TRUE(parsed.fin);
        EXPECT_EQ(parsed.opcode, coop::ws::Opcode::Text);
        EXPECT_EQ(parsed.payload, msg);
    });
}

// -------------------------------------------------------------------------------------
// Ping / Pong — handler sees ping, sends pong
// -------------------------------------------------------------------------------------

TEST(WsTest, PingPong)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendUpgradeRequest(client);

        coop::http::PlaintextTransport httpTransport(server);
        auto httpConn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            httpTransport, ctx, ctx->GetCooperator());
        httpConn->GetRequestLine();
        ASSERT_TRUE(coop::ws::Upgrade(*httpConn));
        RecvAll(client);

        coop::http::PlaintextTransport wsTransport(server);
        auto ws = ctx->Allocate<WsConn>(WS_EXTRA,
            wsTransport, ctx,
            WsConn::DEFAULT_RECV_BUFFER_SIZE,
            WsConn::DEFAULT_SEND_BUFFER_SIZE,
            std::chrono::seconds(5),
            httpConn->LeftoverData(), httpConn->LeftoverSize());

        // Client sends a ping with payload "hi".
        //
        uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
        std::string pingPayload = "hi";
        auto frame = BuildWsFrame(coop::ws::Opcode::Ping, true,
                                   pingPayload.data(), pingPayload.size(), mask);
        SendBytes(client, frame.data(), frame.size());

        // Server sees the ping frame.
        //
        auto* f = ws->NextFrame();
        ASSERT_NE(f, nullptr);
        EXPECT_TRUE(f->IsPing());
        EXPECT_EQ(f->size, 2u);
        EXPECT_EQ(std::string_view(static_cast<const char*>(f->data), f->size), "hi");

        // Server sends pong with same payload.
        //
        EXPECT_TRUE(ws->SendPong(f->data, f->size));

        // Client verifies pong.
        //
        std::string raw = RecvAll(client);
        auto parsed = ParseServerFrame(raw);
        ASSERT_TRUE(parsed.valid);
        EXPECT_EQ(parsed.opcode, coop::ws::Opcode::Pong);
        EXPECT_EQ(parsed.payload, "hi");
    });
}

// -------------------------------------------------------------------------------------
// Close frame
// -------------------------------------------------------------------------------------

TEST(WsTest, CloseFrame)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendUpgradeRequest(client);

        coop::http::PlaintextTransport httpTransport(server);
        auto httpConn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            httpTransport, ctx, ctx->GetCooperator());
        httpConn->GetRequestLine();
        ASSERT_TRUE(coop::ws::Upgrade(*httpConn));
        RecvAll(client);

        coop::http::PlaintextTransport wsTransport(server);
        auto ws = ctx->Allocate<WsConn>(WS_EXTRA,
            wsTransport, ctx,
            WsConn::DEFAULT_RECV_BUFFER_SIZE,
            WsConn::DEFAULT_SEND_BUFFER_SIZE,
            std::chrono::seconds(5),
            httpConn->LeftoverData(), httpConn->LeftoverSize());

        // Client sends close frame with code 1000.
        //
        uint8_t mask[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        uint8_t closePayload[2] = {0x03, 0xE8};  // 1000 in big-endian
        auto frame = BuildWsFrame(coop::ws::Opcode::Close, true,
                                   closePayload, 2, mask);
        SendBytes(client, frame.data(), frame.size());

        auto* f = ws->NextFrame();
        ASSERT_NE(f, nullptr);
        EXPECT_TRUE(f->IsClose());
        EXPECT_TRUE(f->fin);
        EXPECT_TRUE(f->complete);

        // After a close frame, NextFrame returns null.
        //
        EXPECT_EQ(ws->NextFrame(), nullptr);
    });
}

// -------------------------------------------------------------------------------------
// Server Close — sends close frame to client
// -------------------------------------------------------------------------------------

TEST(WsTest, ServerClose)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendUpgradeRequest(client);

        coop::http::PlaintextTransport httpTransport(server);
        auto httpConn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            httpTransport, ctx, ctx->GetCooperator());
        httpConn->GetRequestLine();
        ASSERT_TRUE(coop::ws::Upgrade(*httpConn));
        RecvAll(client);

        coop::http::PlaintextTransport wsTransport(server);
        auto ws = ctx->Allocate<WsConn>(WS_EXTRA,
            wsTransport, ctx,
            WsConn::DEFAULT_RECV_BUFFER_SIZE,
            WsConn::DEFAULT_SEND_BUFFER_SIZE,
            std::chrono::seconds(5),
            httpConn->LeftoverData(), httpConn->LeftoverSize());

        EXPECT_TRUE(ws->Close(1000));

        std::string raw = RecvAll(client);
        auto parsed = ParseServerFrame(raw);
        ASSERT_TRUE(parsed.valid);
        EXPECT_EQ(parsed.opcode, coop::ws::Opcode::Close);
        EXPECT_EQ(parsed.payload.size(), 2u);
        EXPECT_EQ(static_cast<uint8_t>(parsed.payload[0]), 0x03);
        EXPECT_EQ(static_cast<uint8_t>(parsed.payload[1]), 0xE8);
    });
}

// -------------------------------------------------------------------------------------
// Zero-length payload frame
// -------------------------------------------------------------------------------------

TEST(WsTest, EmptyPayload)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendUpgradeRequest(client);

        coop::http::PlaintextTransport httpTransport(server);
        auto httpConn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            httpTransport, ctx, ctx->GetCooperator());
        httpConn->GetRequestLine();
        ASSERT_TRUE(coop::ws::Upgrade(*httpConn));
        RecvAll(client);

        coop::http::PlaintextTransport wsTransport(server);
        auto ws = ctx->Allocate<WsConn>(WS_EXTRA,
            wsTransport, ctx,
            WsConn::DEFAULT_RECV_BUFFER_SIZE,
            WsConn::DEFAULT_SEND_BUFFER_SIZE,
            std::chrono::seconds(5),
            httpConn->LeftoverData(), httpConn->LeftoverSize());

        // Send a text frame with empty payload.
        //
        uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
        auto frame = BuildWsFrame(coop::ws::Opcode::Text, true, nullptr, 0, mask);
        SendBytes(client, frame.data(), frame.size());

        auto* f = ws->NextFrame();
        ASSERT_NE(f, nullptr);
        EXPECT_TRUE(f->IsText());
        EXPECT_TRUE(f->fin);
        EXPECT_TRUE(f->complete);
        EXPECT_EQ(f->size, 0u);
    });
}

// -------------------------------------------------------------------------------------
// Medium payload — 126-byte extended length
// -------------------------------------------------------------------------------------

TEST(WsTest, MediumPayload)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        SocketPair sp;
        auto* uring = coop::GetUring();
        coop::io::Descriptor client(sp.fds[0], uring);
        coop::io::Descriptor server(sp.fds[1], uring);

        SendUpgradeRequest(client);

        coop::http::PlaintextTransport httpTransport(server);
        auto httpConn = ctx->Allocate<HttpConn>(HTTP_EXTRA,
            httpTransport, ctx, ctx->GetCooperator());
        httpConn->GetRequestLine();
        ASSERT_TRUE(coop::ws::Upgrade(*httpConn));
        RecvAll(client);

        coop::http::PlaintextTransport wsTransport(server);
        auto ws = ctx->Allocate<WsConn>(WS_EXTRA,
            wsTransport, ctx,
            WsConn::DEFAULT_RECV_BUFFER_SIZE,
            WsConn::DEFAULT_SEND_BUFFER_SIZE,
            std::chrono::seconds(5),
            httpConn->LeftoverData(), httpConn->LeftoverSize());

        // 300 bytes — triggers 2-byte extended length (len == 126).
        //
        std::string payload(300, 'A');
        uint8_t mask[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        auto frame = BuildWsFrame(coop::ws::Opcode::Text, true,
                                   payload.data(), payload.size(), mask);
        SendBytes(client, frame.data(), frame.size());

        // Reassemble all chunks.
        //
        std::string received;
        while (true)
        {
            auto* f = ws->NextFrame();
            ASSERT_NE(f, nullptr);
            EXPECT_TRUE(f->IsText());
            received.append(static_cast<const char*>(f->data), f->size);
            if (f->complete) break;
        }
        EXPECT_EQ(received, payload);
    });
}
