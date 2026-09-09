#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "coop/io/uring.h"
#include "coop/http/connection.h"
#include "coop/http/detail/server_impl.hpp"
#include "coop/ws/connection.h"
#include "coop/ws/detail/connection_impl.hpp"
#include "coop/ws/upgrade.h"

namespace
{
using coop::ws::Opcode;

struct Script
{
    std::string input;
    std::string sent;
    size_t offset{0};
    size_t stride{1};
    size_t reads{0};
    size_t sends{0};
    int terminal{0};
    bool failSend{false};
    bool shortSend{false};
    coop::io::Descriptor* descriptor{nullptr};
};

struct Transport
{
    static constexpr bool kSpliceable = false;
    Script* script;
    coop::io::Descriptor& Descriptor() { return *script->descriptor; }
    int Recv(void* data, size_t size, int, coop::time::Interval)
    {
        ++script->reads;
        size_t count = std::min({size, script->stride, script->input.size() - script->offset});
        if (!count) return script->terminal;
        memcpy(data, script->input.data() + script->offset, count);
        script->offset += count;
        return static_cast<int>(count);
    }
    int SendAll(const void* data, size_t size)
    {
        ++script->sends;
        if (script->failSend) return -EPIPE;
        if (script->shortSend) return static_cast<int>(size - 1);
        script->sent.append(static_cast<const char*>(data), size);
        return static_cast<int>(size);
    }
    int SendfileAll(int, off_t, size_t) { return -ENOTSUP; }
};

using Ws = coop::ws::Connection<Transport>;
using Http = coop::http::Connection<Transport>;

// Both parsers use their native contiguous trailing buffers and the production CRTP
// implementation. The descriptor exists only to satisfy HTTP's descriptor accessor;
// neither its ring nor any socket is initialized or used.
template<typename Connection>
struct Owned
{
    Connection* ptr;
    explicit Owned(Script& script, size_t recv = 32, size_t send = 64)
    {
        void* storage = ::operator new(sizeof(Connection) + Connection::ExtraBytes(recv, send));
        if constexpr (std::is_same_v<Connection, Ws>)
            ptr = new (storage) Connection(Transport{&script}, nullptr, recv, send);
        else
            ptr = new (storage) Connection(Transport{&script}, nullptr, nullptr, recv, send);
    }
    ~Owned() { ptr->~Connection(); ::operator delete(ptr); }
    Connection* operator->() { return ptr; }
    Connection& operator*() { return *ptr; }
};

std::string Frame(Opcode opcode, bool fin, std::string_view data)
{
    std::string result;
    result += static_cast<char>((fin ? 0x80 : 0) | static_cast<unsigned>(opcode));
    size_t size = data.size();
    if (size <= 125) result += static_cast<char>(0x80 | size);
    else if (size <= 65535)
    {
        result += static_cast<char>(0x80 | 126);
        result += static_cast<char>(size >> 8);
        result += static_cast<char>(size);
    }
    else
    {
        result += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i) result += static_cast<char>(size >> (8 * i));
    }
    constexpr char mask[] = {0x12, 0x34, 0x56, 0x78};
    result.append(mask, 4);
    for (size_t i = 0; i < size; ++i) result += data[i] ^ mask[i & 3];
    return result;
}

TEST(WsParserTest, ClosePayloadSurvivesEveryReceiveBoundary)
{
    std::string payload("\x03\xe8", 2);
    payload += std::string(123, 'x');
    for (size_t stride = 1; stride < 135; ++stride)
    {
        SCOPED_TRACE(stride);
        Script script{Frame(Opcode::Close, true, payload) + Frame(Opcode::Text, true, "after")};
        script.stride = stride;
        Owned<Ws> ws(script, 12);
        std::string got;
        while (auto* frame = ws->NextFrame())
        {
            EXPECT_TRUE(frame->IsClose());
            EXPECT_GE(frame->data, ws->m_buf);
            EXPECT_LT(frame->data, ws->m_buf + ws->m_recvBufSize);
            got.append(static_cast<const char*>(frame->data), frame->size);
            EXPECT_EQ(frame->complete, got.size() == payload.size());
            EXPECT_EQ(ws->PeerClosed(), frame->complete);
        }
        EXPECT_EQ(got, payload);
        EXPECT_TRUE(ws->PeerClosed());
        EXPECT_EQ(ws->Error(), 0);
    }
}

TEST(WsParserTest, SkipRemainingClosePayload)
{
    Script script{Frame(Opcode::Close, true, std::string("\x03\xe8reason", 8))};
    Owned<Ws> ws(script);
    auto* frame = ws->NextFrame();
    ASSERT_NE(frame, nullptr);
    ASSERT_FALSE(frame->complete);
    ws->SkipPayload();
    EXPECT_TRUE(ws->PeerClosed());
    size_t reads = script.reads;
    EXPECT_EQ(ws->NextFrame(), nullptr);
    EXPECT_EQ(script.reads, reads);
}

TEST(WsParserTest, TruncatedCloseIsNotPeerClosure)
{
    Script script{Frame(Opcode::Close, true, std::string("\x03\xe8reason", 8))};
    script.input.pop_back();
    Owned<Ws> ws(script);
    while (ws->NextFrame()) {}
    EXPECT_FALSE(ws->PeerClosed());
    EXPECT_EQ(ws->Error(), -ECONNRESET);
    size_t reads = script.reads;
    EXPECT_EQ(ws->NextFrame(), nullptr);
    EXPECT_EQ(script.reads, reads);
}

TEST(WsParserTest, NativeTransportErrorPreserved)
{
    Script script;
    script.terminal = -ECANCELED;
    Owned<Ws> ws(script);
    EXPECT_EQ(ws->NextFrame(), nullptr);
    EXPECT_EQ(ws->Error(), -ECANCELED);
}

TEST(WsParserTest, LargeFramesStreamThroughSmallBuffer)
{
    std::string payload(70000, 'a');
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i);
    Script script{Frame(Opcode::Binary, true, payload)};
    script.stride = 19;
    Owned<Ws> ws(script, 12);
    std::string got;
    while (got.size() < payload.size())
    {
        auto* frame = ws->NextFrame();
        ASSERT_NE(frame, nullptr);
        got.append(static_cast<const char*>(frame->data), frame->size);
    }
    EXPECT_EQ(got, payload);
    EXPECT_EQ(ws->Error(), 0);
}

TEST(WsParserTest, RejectsNonminimalLengthsWithoutAutomaticReplyWhenDisabled)
{
    for (auto bytes : {std::string("\x82\xfe\0\x01", 4),
                       std::string("\x82\xff\0\0\0\0\0\0\0\x7e", 10)})
    {
        Script script{bytes + std::string(4, '\0')};
        Owned<Ws> ws(script);
        ws->SetAutoCloseOnError(false);
        EXPECT_EQ(ws->NextFrame(), nullptr);
        EXPECT_EQ(ws->ProtocolError(), 1002);
        EXPECT_EQ(ws->Error(), -EPROTO);
        EXPECT_TRUE(script.sent.empty());
        EXPECT_TRUE(ws->Close(1002));
        EXPECT_EQ(script.sent.size(), 4);
    }
}

TEST(WsParserTest, ControlFramesDoNotConsumeDataMessageBudget)
{
    Script script{Frame(Opcode::Text, false, "a") + Frame(Opcode::Ping, true, "12345")
                + Frame(Opcode::Continuation, true, "b")};
    script.stride = 100;
    Owned<Ws> ws(script, 64);
    ws->SetMaxMessageSize(2);
    auto* first = ws->NextFrame();
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(first->IsText());
    auto* ping = ws->NextFrame();
    ASSERT_NE(ping, nullptr);
    EXPECT_TRUE(ping->IsPing());
    auto* last = ws->NextFrame();
    ASSERT_NE(last, nullptr);
    EXPECT_TRUE(last->IsText());
    EXPECT_TRUE(last->fin);
    EXPECT_EQ(ws->ProtocolError(), 0);
}

TEST(WsParserTest, CumulativeMessageBudgetAndCallerRaisedLimit)
{
    Script script{Frame(Opcode::Text, false, "ab") + Frame(Opcode::Continuation, true, "cd")};
    script.stride = 100;
    Owned<Ws> ws(script, 64);
    ws->SetMaxMessageSize(3);
    ASSERT_NE(ws->NextFrame(), nullptr);
    EXPECT_EQ(ws->NextFrame(), nullptr);
    EXPECT_EQ(ws->ProtocolError(), 1009);

    // Advertise a larger message and deliver only its first byte. Admission must not
    // allocate or demand the entire declared body before returning that borrowed span.
    std::string largeHeader("\x82\xff", 2);
    uint64_t declaredSize = 17 * 1024 * 1024;
    for (int i = 7; i >= 0; --i) largeHeader += static_cast<char>(declaredSize >> (8 * i));
    largeHeader.append(4, '\0');
    Script raised{largeHeader + "a"};
    raised.stride = 128;
    Owned<Ws> larger(raised, 128);
    larger->SetMaxMessageSize(std::numeric_limits<size_t>::max());
    EXPECT_NE(larger->NextFrame(), nullptr);
    EXPECT_EQ(larger->ProtocolError(), 0);
}

TEST(WsParserTest, ExplicitOutboundFragmentsAndControlInterleaving)
{
    Script script;
    Owned<Ws> ws(script);
    EXPECT_FALSE(ws->SendFragment(Opcode::Continuation, true, "x", 1));
    EXPECT_TRUE(ws->SendFragment(Opcode::Text, false, "a", 1));
    EXPECT_FALSE(ws->SendBinary("x", 1));
    EXPECT_TRUE(ws->SendPing("p", 1));
    EXPECT_TRUE(ws->SendFragment(Opcode::Continuation, true, "b", 1));
    EXPECT_EQ(script.sent, std::string("\x01\x01" "a" "\x89\x01" "p" "\x80\x01" "b", 9));
    EXPECT_TRUE(ws->Close(1000));
    size_t sends = script.sends;
    EXPECT_FALSE(ws->SendText("x", 1));
    EXPECT_FALSE(ws->SendPing(nullptr, 0));
    EXPECT_EQ(script.sends, sends);
}

TEST(WsParserTest, SendFailuresAreStickyIncludingRepeatedClose)
{
    for (bool shortSend : {false, true})
    {
        Script script;
        script.failSend = !shortSend;
        script.shortSend = shortSend;
        Owned<Ws> ws(script);
        EXPECT_FALSE(ws->Close(1000));
        EXPECT_TRUE(ws->SendError());
        size_t sends = script.sends;
        EXPECT_FALSE(ws->Close(1000));
        EXPECT_FALSE(ws->SendText("x", 1));
        EXPECT_EQ(script.sends, sends);
    }
}

TEST(WsParserTest, SendingPreservesBorrowedReceiveSpan)
{
    Script script{Frame(Opcode::Binary, true, "borrowed bytes")};
    script.stride = 128;
    Owned<Ws> ws(script, 64, 2);
    auto* frame = ws->NextFrame();
    ASSERT_NE(frame, nullptr);
    const void* data = frame->data;
    size_t size = frame->size;
    EXPECT_TRUE(ws->SendBinary(data, size));
    EXPECT_EQ(frame->data, data);
    EXPECT_EQ(std::string_view(static_cast<const char*>(data), size), "borrowed bytes");
}

std::string Handshake(std::string fields = {})
{
    return "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
           "Connection: keep-alive, Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\n" + fields + "\r\n";
}

class WsUpgradeParserTest : public testing::Test
{
  protected:
    coop::io::Uring ring;
    coop::io::Descriptor descriptor{coop::io::borrowed, -1, &ring};
};

TEST_F(WsUpgradeParserTest, EveryReceiveBoundaryAndLeftoverFrame)
{
    for (size_t stride = 1; stride < 220; ++stride)
    {
        SCOPED_TRACE(stride);
        Script script{Handshake() + Frame(Opcode::Text, true, "hello")};
        script.stride = stride;
        script.descriptor = &descriptor;
        Owned<Http> http(script, 64);
        ASSERT_TRUE(coop::ws::Upgrade(*http));
        EXPECT_FALSE(http->KeepAlive());
        EXPECT_FALSE(http->Reusable());
        ASSERT_NE(script.sent.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos) << script.sent;
        size_t recv = 64;
        void* storage = ::operator new(sizeof(Ws) + Ws::ExtraBytes(recv, 64));
        auto* ws = new (storage) Ws(Transport{&script}, nullptr, recv, 64,
                                    std::chrono::seconds(30),
                                    http->LeftoverData(), http->LeftoverSize());
        std::string got;
        while (got.size() < 5)
        {
            auto* frame = ws->NextFrame();
            ASSERT_NE(frame, nullptr);
            got.append(static_cast<const char*>(frame->data), frame->size);
        }
        EXPECT_EQ(got, "hello");
        ws->~Ws();
        ::operator delete(storage);
    }
}

TEST_F(WsUpgradeParserTest, LongTokenListsStreamWithoutAccumulation)
{
    std::string request = Handshake();
    auto pos = request.find("keep-alive, Upgrade");
    request.replace(pos, strlen("keep-alive, Upgrade"), std::string(4000, 'x') + ", UpGrAdE");
    Script script{request};
    script.descriptor = &descriptor;
    Owned<Http> http(script, 64);
    EXPECT_TRUE(coop::ws::Upgrade(*http));
}

TEST_F(WsUpgradeParserTest, InvalidSingleValuesAndMethodRejected)
{
    std::vector<std::string> requests{
        Handshake("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"),
        Handshake("Sec-WebSocket-Version: 13\r\n"),
        Handshake("Content-Length: 1\r\n"),
        Handshake("Transfer-Encoding: chunked\r\n") + "0\r\n\r\n",
    };
    auto badKey = Handshake();
    badKey.replace(badKey.find("dGhlIHNhbXBsZSBub25jZQ=="), 24, "aaaaaaaaaaaaaaaaaaaaaa==");
    requests.push_back(badKey);
    auto post = Handshake();
    post.replace(0, 3, "POST");
    requests.push_back(post);
    auto http10 = Handshake();
    http10.replace(http10.find("HTTP/1.1"), 8, "HTTP/1.0");
    requests.push_back(http10);
    for (const auto& request : requests)
    {
        SCOPED_TRACE(request);
        Script script{request};
        script.descriptor = &descriptor;
        Owned<Http> http(script, 64);
        EXPECT_FALSE(coop::ws::Upgrade(*http, false));
        EXPECT_TRUE(script.sent.empty());
    }
}

TEST_F(WsUpgradeParserTest, InvalidHandshakeDefaultReplyAndSendFailure)
{
    Script script{Handshake("Sec-WebSocket-Version: 13\r\n")};
    script.descriptor = &descriptor;
    Owned<Http> http(script, 64);
    EXPECT_FALSE(coop::ws::Upgrade(*http));
    EXPECT_EQ(script.sent.find("HTTP/1.1 400"), 0);
    EXPECT_NE(script.sent.find("Content-Length: 24"), std::string::npos);
    EXPECT_FALSE(http->KeepAlive());

    Script failed{Handshake()};
    failed.descriptor = &descriptor;
    failed.failSend = true;
    Owned<Http> failedHttp(failed, 64);
    EXPECT_FALSE(coop::ws::Upgrade(*failedHttp));
    EXPECT_TRUE(failedHttp->SendError());
    EXPECT_FALSE(failedHttp->KeepAlive());
}

TEST_F(WsUpgradeParserTest, TruncatedHeaderBlockNeverSendsSwitchingProtocols)
{
    Script script{Handshake()};
    script.input.resize(script.input.size() - 2);
    script.descriptor = &descriptor;
    Owned<Http> http(script, 64);
    EXPECT_FALSE(coop::ws::Upgrade(*http));
    EXPECT_EQ(script.sent.find("101 Switching"), std::string::npos);
    EXPECT_NE(http->Error(), 0);
}
} // namespace
