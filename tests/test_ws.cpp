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

// =====================================================================================
// Frame header validation
//
// Every field in a WebSocket frame header — FIN, RSV, opcode, MASK, and the length in
// three widths — is written by the peer. Each case here pairs a header that must fail
// the connection with a well-formed one that must still be delivered.
// =====================================================================================

namespace
{

// Build a frame with direct control over the header. `b0` carries FIN, RSV and opcode.
// `lengthForm` picks the width the length is written in (0 = shortest that fits, 2 =
// 16-bit, 8 = 64-bit), and `declaredLen` is the length the header names — which need not
// be the number of payload bytes that follow.
//
std::string BuildRawWsFrame(uint8_t b0, bool masked, uint64_t declaredLen, int lengthForm,
                            const void* payload, size_t payloadSize,
                            const uint8_t maskKey[4])
{
    std::string frame;
    frame.push_back(static_cast<char>(b0));

    uint8_t maskBit = masked ? 0x80 : 0x00;

    if (lengthForm == 0 && declaredLen <= 125)
    {
        frame.push_back(static_cast<char>(maskBit | declaredLen));
    }
    else if (lengthForm == 2 || (lengthForm == 0 && declaredLen <= 65535))
    {
        frame.push_back(static_cast<char>(maskBit | 126));
        frame.push_back(static_cast<char>((declaredLen >> 8) & 0xFF));
        frame.push_back(static_cast<char>(declaredLen & 0xFF));
    }
    else
    {
        frame.push_back(static_cast<char>(maskBit | 127));
        for (int i = 7; i >= 0; i--)
            frame.push_back(static_cast<char>((declaredLen >> (8 * i)) & 0xFF));
    }

    if (masked) frame.append(reinterpret_cast<const char*>(maskKey), 4);

    auto* src = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < payloadSize; i++)
    {
        frame.push_back(static_cast<char>(masked ? (src[i] ^ maskKey[i & 3]) : src[i]));
    }
    return frame;
}

const uint8_t kMask[4] = {0x37, 0xfa, 0x21, 0x3d};

// Upgrade a connection, feed it `clientBytes`, and hand the ws connection to `fn`.
//
template<typename Fn>
void WithUpgradedWs(coop::Context* ctx, const std::string& clientBytes, Fn&& fn)
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

    if (!clientBytes.empty())
    {
        SendBytes(client, clientBytes.data(), clientBytes.size());
    }

    coop::http::PlaintextTransport wsTransport(server);
    auto ws = ctx->Allocate<WsConn>(WS_EXTRA,
        wsTransport, ctx,
        WsConn::DEFAULT_RECV_BUFFER_SIZE,
        WsConn::DEFAULT_SEND_BUFFER_SIZE,
        std::chrono::milliseconds(200),
        httpConn->LeftoverData(), httpConn->LeftoverSize());

    fn(*ws, client);
}

// A rejected frame must be refused *and* explained: the peer gets a close frame carrying
// the code before the stream ends.
//
void ExpectClosedWith(coop::ws::ConnectionBase& ws, coop::io::Descriptor& client,
                      uint16_t code)
{
    EXPECT_EQ(ws.NextFrame(), nullptr);
    EXPECT_EQ(ws.ProtocolError(), code);

    ParsedFrame sent = ParseServerFrame(RecvAll(client));
    ASSERT_TRUE(sent.valid);
    EXPECT_EQ(sent.opcode, coop::ws::Opcode::Close);
    ASSERT_EQ(sent.payload.size(), 2u);
    uint16_t sentCode = (static_cast<uint8_t>(sent.payload[0]) << 8)
                      | static_cast<uint8_t>(sent.payload[1]);
    EXPECT_EQ(sentCode, code);
}

} // end anonymous namespace

// A control frame is at most 125 bytes and is never fragmented (RFC 6455 5.5). This is
// the rule that makes echoing a Ping's payload back from a fixed buffer safe — without
// it a 4096-byte Ping reaches a handler that then tries to Pong it.
//
TEST(WsFramingTest, OversizedPingRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string payload(126, 'p');
        std::string frame = BuildRawWsFrame(0x89, true, payload.size(), 0,
                                            payload.data(), payload.size(), kMask);

        WithUpgradedWs(ctx, frame, [](coop::ws::ConnectionBase& ws,
                                      coop::io::Descriptor& client)
        {
            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_PROTOCOL_ERROR);
        });
    });
}

TEST(WsFramingTest, MaximumSizePingAccepted)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string payload(125, 'p');
        std::string frame = BuildRawWsFrame(0x89, true, payload.size(), 0,
                                            payload.data(), payload.size(), kMask);

        WithUpgradedWs(ctx, frame, [&payload](coop::ws::ConnectionBase& ws,
                                              coop::io::Descriptor&)
        {
            auto* f = ws.NextFrame();
            ASSERT_NE(f, nullptr);
            EXPECT_TRUE(f->IsPing());
            EXPECT_TRUE(f->complete);
            EXPECT_EQ(std::string(static_cast<const char*>(f->data), f->size), payload);

            // A handler echoing the Ping is exactly what this bound is for.
            //
            EXPECT_TRUE(ws.SendPong(f->data, f->size));
        });
    });
}

// SendPong refuses an oversized payload instead of asserting: the size comes from a
// Ping the peer sent, so aborting on it hands the process to the peer.
//
TEST(WsFramingTest, SendPongRefusesOversizedPayload)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        WithUpgradedWs(ctx, "", [](coop::ws::ConnectionBase& ws, coop::io::Descriptor&)
        {
            std::string payload(126, 'p');
            EXPECT_FALSE(ws.SendPong(payload.data(), payload.size()));
            EXPECT_TRUE(ws.SendPong(payload.data(), 125));
        });
    });
}

TEST(WsFramingTest, ControlFrameFragmentRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        // Ping with FIN clear.
        //
        std::string frame = BuildRawWsFrame(0x09, true, 0, 0, nullptr, 0, kMask);

        WithUpgradedWs(ctx, frame, [](coop::ws::ConnectionBase& ws,
                                      coop::io::Descriptor& client)
        {
            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_PROTOCOL_ERROR);
        });
    });
}

// A client-to-server frame must be masked (RFC 6455 5.1). Accepting an unmasked one lets
// a peer choose the literal bytes an intermediary on the path sees.
//
TEST(WsFramingTest, UnmaskedClientFrameRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string payload = "hello";
        std::string frame = BuildRawWsFrame(0x81, false, payload.size(), 0,
                                            payload.data(), payload.size(), kMask);

        WithUpgradedWs(ctx, frame, [](coop::ws::ConnectionBase& ws,
                                      coop::io::Descriptor& client)
        {
            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_PROTOCOL_ERROR);
        });
    });
}

// RSV bits belong to an extension the handshake negotiated. None is offered here, so a
// set bit means the peer is framing to a grammar this parser does not read.
//
TEST(WsFramingTest, ReservedBitRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string payload = "hello";
        std::string frame = BuildRawWsFrame(0xC1, true, payload.size(), 0,
                                            payload.data(), payload.size(), kMask);

        WithUpgradedWs(ctx, frame, [](coop::ws::ConnectionBase& ws,
                                      coop::io::Descriptor& client)
        {
            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_PROTOCOL_ERROR);
        });
    });
}

// Opcodes 3-7 and 11-15 are reserved. Delivering one under whatever opcode was last seen
// lets the peer decide what type its bytes are.
//
TEST(WsFramingTest, ReservedOpcodeRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string payload = "hello";
        std::string frame = BuildRawWsFrame(0x83, true, payload.size(), 0,
                                            payload.data(), payload.size(), kMask);

        WithUpgradedWs(ctx, frame, [](coop::ws::ConnectionBase& ws,
                                      coop::io::Descriptor& client)
        {
            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_PROTOCOL_ERROR);
        });
    });
}

// The most significant bit of a 64-bit length must be 0 (RFC 6455 5.2).
//
TEST(WsFramingTest, SixtyFourBitLengthWithHighBitRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string frame = BuildRawWsFrame(0x82, true, 0x8000000000000000ull, 8,
                                            nullptr, 0, kMask);

        WithUpgradedWs(ctx, frame, [](coop::ws::ConnectionBase& ws,
                                      coop::io::Descriptor& client)
        {
            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_PROTOCOL_ERROR);
        });
    });
}

// A length the connection will not hold is refused from the header, before a byte of the
// payload is read.
//
TEST(WsFramingTest, PayloadAboveCeilingRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        uint64_t declared = coop::ws::ConnectionBase::DEFAULT_MAX_MESSAGE_SIZE + 1;
        std::string frame = BuildRawWsFrame(0x82, true, declared, 8, nullptr, 0, kMask);

        WithUpgradedWs(ctx, frame, [](coop::ws::ConnectionBase& ws,
                                      coop::io::Descriptor& client)
        {
            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_MESSAGE_TOO_BIG);
        });
    });
}

// The ceiling applies to a message assembled from fragments, not only to one frame.
//
TEST(WsFramingTest, FragmentedMessageAboveCeilingRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string first = "abcd";
        std::string frames = BuildRawWsFrame(0x01, true, first.size(), 0,
                                             first.data(), first.size(), kMask);
        frames += BuildRawWsFrame(0x80, true,
                                  coop::ws::ConnectionBase::DEFAULT_MAX_MESSAGE_SIZE, 8,
                                  nullptr, 0, kMask);

        WithUpgradedWs(ctx, frames, [](coop::ws::ConnectionBase& ws,
                                       coop::io::Descriptor& client)
        {
            auto* f = ws.NextFrame();
            ASSERT_NE(f, nullptr);
            EXPECT_TRUE(f->IsText());

            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_MESSAGE_TOO_BIG);
        });
    });
}

// A Continuation names a message an earlier non-final data frame opened. With none open
// its bytes have no message to join, and reporting them under the last opcode seen makes
// the peer the author of what type they are.
//
TEST(WsFramingTest, StrayContinuationRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string payload = "orphan";
        std::string frame = BuildRawWsFrame(0x80, true, payload.size(), 0,
                                            payload.data(), payload.size(), kMask);

        WithUpgradedWs(ctx, frame, [](coop::ws::ConnectionBase& ws,
                                      coop::io::Descriptor& client)
        {
            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_PROTOCOL_ERROR);
        });
    });
}

// A second data frame while a message is still open would interleave two messages in one
// stream (RFC 6455 5.4).
//
TEST(WsFramingTest, DataFrameDuringOpenMessageRejected)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string first = "Hello";
        std::string second = "World";
        std::string frames = BuildRawWsFrame(0x01, true, first.size(), 0,
                                             first.data(), first.size(), kMask);
        frames += BuildRawWsFrame(0x81, true, second.size(), 0,
                                  second.data(), second.size(), kMask);

        WithUpgradedWs(ctx, frames, [](coop::ws::ConnectionBase& ws,
                                       coop::io::Descriptor& client)
        {
            auto* f = ws.NextFrame();
            ASSERT_NE(f, nullptr);
            EXPECT_TRUE(f->IsText());
            EXPECT_FALSE(f->fin);

            ExpectClosedWith(ws, client, coop::ws::ConnectionBase::CLOSE_PROTOCOL_ERROR);
        });
    });
}

// The rules above must not cost a conforming client its fragmented messages.
//
TEST(WsFramingTest, FragmentedTextMessageDelivered)
{
    test::RunInCooperator([](coop::Context* ctx)
    {
        std::string first = "Hello, ";
        std::string mid = "frag";
        std::string last = "mented!";

        std::string frames = BuildRawWsFrame(0x01, true, first.size(), 0,
                                             first.data(), first.size(), kMask);
        frames += BuildRawWsFrame(0x00, true, mid.size(), 0,
                                  mid.data(), mid.size(), kMask);
        frames += BuildRawWsFrame(0x80, true, last.size(), 0,
                                  last.data(), last.size(), kMask);

        WithUpgradedWs(ctx, frames, [](coop::ws::ConnectionBase& ws,
                                       coop::io::Descriptor&)
        {
            std::string message;
            bool fin = false;
            while (!fin)
            {
                auto* f = ws.NextFrame();
                ASSERT_NE(f, nullptr);
                EXPECT_TRUE(f->IsText());     // fragments report the message's opcode
                message.append(static_cast<const char*>(f->data), f->size);
                fin = f->fin && f->complete;
            }
            EXPECT_EQ(message, "Hello, fragmented!");
            EXPECT_EQ(ws.ProtocolError(), 0);

            // The message closed cleanly, so the next one may start.
            //
            EXPECT_TRUE(ws.SendText("ok", 2));
        });
    });
}
