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
#include <utility>
#include <vector>

#include "coop/http/client.h"
#include "coop/http/detail/client_impl.hpp"

namespace
{

// Exercise the production parser with a compile-time transport. Packet boundaries and
// terminal transport results are deterministic; no cooperator, socket or io_uring is used.
// The transport records destination spans to check that returned flyweights borrow the
// receive storage rather than an independently materialized body.
//
struct Script
{
    std::vector<std::string> packets;
    size_t packet{0};
    size_t offset{0};
    size_t reads{0};
    size_t terminalReads{0};
    int terminalResult{0};
    int sendResult{0};
    size_t sends{0};
    size_t fileFailAt{std::numeric_limits<size_t>::max()};
    int fileError{-EPIPE};
    std::vector<std::pair<off_t, size_t>> fileSends;
    std::string sent;
    std::vector<std::pair<const char*, size_t>> destinations;
};

struct ScriptTransport
{
    static constexpr bool kSpliceable = false;
    Script* script;

    int Recv(void* data, size_t size, int, coop::time::Interval)
    {
        ++script->reads;
        if (script->packet == script->packets.size())
        {
            // Turn an EOF spin into a test failure rather than wedging the test runner.
            //
            if (++script->terminalReads > 2)
            {
                throw std::runtime_error("parser repeatedly received after terminal result");
            }
            return script->terminalResult;
        }
        auto& packet = script->packets[script->packet];
        size_t count = std::min(size, packet.size() - script->offset);
        if (count == 0) throw std::runtime_error("zero-capacity recv or empty script packet");
        memcpy(data, packet.data() + script->offset, count);
        script->destinations.emplace_back(static_cast<const char*>(data), count);
        script->offset += count;
        if (script->offset == packet.size())
        {
            ++script->packet;
            script->offset = 0;
        }
        return static_cast<int>(count);
    }

    int SendAll(const void* data, size_t size)
    {
        ++script->sends;
        if (script->sendResult < 0) return script->sendResult;
        script->sent.append(static_cast<const char*>(data), size);
        return static_cast<int>(size);
    }

    int SendfileAll(int, off_t offset, size_t count)
    {
        script->fileSends.emplace_back(offset, count);
        if (script->fileSends.size() - 1 == script->fileFailAt) return script->fileError;
        // This transport, like production transports, reports counts through an int.
        //
        EXPECT_LE(count, static_cast<size_t>(std::numeric_limits<int>::max()));
        return static_cast<int>(count);
    }
};

using Client = coop::http::ClientConnection<ScriptTransport>;
using BodyResult = decltype(std::declval<Client&>().NextBody());
static_assert(std::is_trivially_copyable_v<BodyResult>);
static_assert(sizeof(BodyResult) <= 2 * sizeof(void*));

struct Parser
{
    Script script;
    Client* client;

    explicit Parser(std::vector<std::string> packets, size_t recvSize = 128,
                    size_t sendSize = 64)
    : script{std::move(packets)}
    , client(new (::operator new(sizeof(Client) + Client::ExtraBytes(recvSize, sendSize)))
          Client(ScriptTransport{&script}, "example.test", recvSize, sendSize))
    {}

    ~Parser()
    {
        client->~Client();
        ::operator delete(client);
    }

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    std::string Body()
    {
        std::string body;
        while (auto* chunk = client->ReadBody())
        {
            body.append(static_cast<const char*>(chunk->data), chunk->size);
        }
        return body;
    }

    std::vector<std::pair<std::string, std::string>> Headers()
    {
        std::vector<std::pair<std::string, std::string>> headers;
        while (const char* name = client->NextHeaderName())
        {
            // A name is borrowed; preserve it before the next parsing operation.
            //
            headers.emplace_back(name, "");
            while (auto* value = client->ReadHeaderValue())
            {
                headers.back().second.append(static_cast<const char*>(value->data), value->size);
                if (value->complete) break;
            }
        }
        return headers;
    }
};

std::vector<std::string> Bytes(const std::string& input)
{
    std::vector<std::string> packets;
    for (char byte : input) packets.emplace_back(1, byte);
    return packets;
}

const std::string kNextResponse = "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";

void ExpectReusableResponse(Parser& parser, std::string_view expected)
{
    EXPECT_EQ(parser.Body(), expected);
    EXPECT_TRUE(parser.client->Complete());
    EXPECT_EQ(parser.client->Error(), 0);
    EXPECT_TRUE(parser.client->Reusable());
}


TEST(HttpClientParser, CheckedBodyPullDistinguishesDataCompletionAndFailure)
{
    Parser success({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
    auto data = success.client->NextBody();
    ASSERT_TRUE(data);
    EXPECT_FALSE(data.Complete());
    EXPECT_EQ(data.Error(), 0);
    EXPECT_EQ(std::string_view(static_cast<const char*>(data->data), data->size), "ok");
    auto end = success.client->NextBody();
    EXPECT_FALSE(end);
    EXPECT_TRUE(end.Complete());
    EXPECT_EQ(end.Error(), 0);

    Parser failure({"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhi"});
    ASSERT_TRUE(failure.client->NextBody());
    auto truncated = failure.client->NextBody();
    EXPECT_FALSE(truncated);
    EXPECT_FALSE(truncated.Complete());
    EXPECT_EQ(truncated.Error(), -ECONNRESET);
    EXPECT_FALSE(failure.client->SkipBody());
}


TEST(HttpClientParser, NoBodyCheckedPullReturnsTerminalSuccessWithoutExtraReceive)
{
    for (const std::string& response : {
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 204 No Content\r\n\r\n",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 999\r\n\r\n"})
    {
        Parser parser({response});
        auto end = parser.client->NextBody();
        EXPECT_FALSE(end);
        EXPECT_TRUE(end.Complete());
        EXPECT_EQ(end.Error(), 0);
        EXPECT_TRUE(parser.client->Reusable());
        EXPECT_EQ(parser.script.reads, 1);
        EXPECT_EQ(parser.script.terminalReads, 0);
    }
}

TEST(HttpClientParser, WireChunkCompletionDoesNotImplyMessageCompletion)
{
    Parser parser({"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "1\r\na", "\r\n1\r\nb\r\n0\r\n\r\n"});
    auto first = parser.client->NextBody();
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->complete);
    EXPECT_FALSE(first.Complete());
    EXPECT_FALSE(parser.client->Complete());
    EXPECT_FALSE(parser.client->Reusable());
    // Returning the last span of a wire chunk must not receive its trailing delimiter.
    // The caller owns pacing and the returned bytes are live until the next pull.
    //
    EXPECT_EQ(parser.script.reads, 1);
    EXPECT_EQ(std::string_view(static_cast<const char*>(first->data), first->size), "a");
    auto second = parser.client->NextBody();
    ASSERT_TRUE(second);
    EXPECT_EQ(std::string_view(static_cast<const char*>(second->data), second->size), "b");
    EXPECT_FALSE(second.Complete());
    EXPECT_TRUE(parser.client->NextBody().Complete());
    EXPECT_TRUE(parser.client->Reusable());
}

TEST(HttpClientParser, ResetDoesNotConsumeIncompleteResponseOrEraseFailure)
{
    Parser parser({"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhi"});
    EXPECT_FALSE(parser.client->Reusable());
    EXPECT_FALSE(parser.client->Reset());
    EXPECT_EQ(parser.script.reads, 0);
    ASSERT_NE(parser.client->ReadBody(), nullptr);
    EXPECT_FALSE(parser.client->Complete());
    EXPECT_FALSE(parser.client->Reset());
    EXPECT_EQ(parser.client->ReadBody(), nullptr);
    EXPECT_EQ(parser.client->Error(), -ECONNRESET);
    EXPECT_FALSE(parser.client->Reset());
    EXPECT_EQ(parser.client->Error(), -ECONNRESET);
}

TEST(HttpClientParser, InformationalResponsesRemainUnderCallerControl)
{
    Parser parser(Bytes("HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n"
                        "HTTP/1.1 100 Continue\r\n\r\n"
                        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n"), 64);
    // HEAD has to remain HEAD across informational responses.
    //
    ASSERT_TRUE(parser.client->Head("/"));
    auto* first = parser.client->GetResponseLine();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->status, 103);
    EXPECT_EQ(parser.Headers(), (std::vector<std::pair<std::string, std::string>>{
        {"Link", "</style.css>"}}));
    EXPECT_TRUE(parser.client->SkipBody());
    EXPECT_FALSE(parser.client->Reusable());
    EXPECT_FALSE(parser.client->Reset());
    ASSERT_TRUE(parser.client->AdvanceResponse());
    auto* second = parser.client->GetResponseLine();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->status, 100);
    EXPECT_TRUE(parser.client->SkipBody());
    ASSERT_TRUE(parser.client->AdvanceResponse());
    auto* final = parser.client->GetResponseLine();
    ASSERT_NE(final, nullptr);
    EXPECT_EQ(final->status, 200);
    EXPECT_EQ(parser.Body(), "");
    EXPECT_TRUE(parser.client->Complete());
    EXPECT_TRUE(parser.client->Reusable());
    EXPECT_FALSE(parser.client->AdvanceResponse());
    EXPECT_EQ(parser.script.terminalReads, 0);
}

TEST(HttpClientParser, ContentLengthIndependentOfEveryReceiveBoundary)
{
    const std::string body = "hello\r\nworld" + std::string(200, 'x');
    const std::string response = "HTTP/1.1 200 OK\r\nX-Test: one two\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    for (size_t split = 1; split < response.size(); ++split)
    {
        SCOPED_TRACE(split);
        Parser parser({response.substr(0, split), response.substr(split)}, 64);
        ExpectReusableResponse(parser, body);
    }
    Parser parser(Bytes(response), 64);
    ExpectReusableResponse(parser, body);
}

TEST(HttpClientParser, ChunkedIndependentOfEveryReceiveBoundary)
{
    const std::string body = "hello" + std::string(80, 'B');
    const std::string response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5;key=value\r\nhello\r\n50\r\n" + std::string(80, 'B') +
        "\r\n0\r\nChecksum: yes\r\n\r\n";
    for (size_t split = 1; split < response.size(); ++split)
    {
        SCOPED_TRACE(split);
        Parser parser({response.substr(0, split), response.substr(split)}, 64);
        ExpectReusableResponse(parser, body);
    }
    Parser parser(Bytes(response), 64);
    ExpectReusableResponse(parser, body);
}

TEST(HttpClientParser, HeaderIterationAndSkippingHaveIdenticalFraming)
{
    const std::string response = "HTTP/1.1 200 OK\r\nX: a\r\nContent-Length: 5\r\n"
        "Connection: keep-alive\r\n\r\nhello";
    for (size_t split = 1; split < response.size(); ++split)
    {
        SCOPED_TRACE(split);
        Parser read({response.substr(0, split), response.substr(split)}, 64);
        auto headers = read.Headers();
        ASSERT_EQ(headers.size(), 3);
        EXPECT_EQ(headers[0], std::make_pair(std::string("X"), std::string("a")));
        EXPECT_EQ(headers[1].second, "5");
        ExpectReusableResponse(read, "hello");
        Parser skip({response.substr(0, split), response.substr(split)}, 64);
        skip.client->SkipHeaders();
        ExpectReusableResponse(skip, "hello");
    }
}

TEST(HttpClientParser, HeaderValueBorrowIsValidUntilNextParsingOperation)
{
    const std::string value = std::string(45, 'A') + std::string(300, 'B');
    const std::string response = "HTTP/1.1 200 OK\r\nX: " + value +
        "\r\nContent-Length: 0\r\n\r\n";
    Parser parser({response}, 64);
    ASSERT_NE(parser.client->GetResponseLine(), nullptr);
    ASSERT_STREQ(parser.client->NextHeaderName(), "X");
    std::string collected;
    size_t chunks = 0;
    while (auto* chunk = parser.client->ReadHeaderValue())
    {
        // Inspect each borrow immediately. Refilling before returning corrupts this
        // prefix even if later chunks eventually reproduce the right total length.
        //
        std::string_view borrowed(static_cast<const char*>(chunk->data), chunk->size);
        ASSERT_EQ(borrowed, std::string_view(value).substr(collected.size(), chunk->size));
        collected.append(borrowed);
        ++chunks;
        if (chunk->complete) break;
    }
    EXPECT_GT(chunks, 1);
    EXPECT_EQ(collected, value);
    ExpectReusableResponse(parser, "");
}

TEST(HttpClientParser, BodyFlyweightBorrowsReceiveStorage)
{
    Parser parser({"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"});
    auto* chunk = parser.client->ReadBody();
    ASSERT_NE(chunk, nullptr);
    ASSERT_EQ(chunk->size, 5);
    EXPECT_EQ(std::string_view(static_cast<const char*>(chunk->data), chunk->size), "hello");
    bool borrowed = false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(chunk->data);
    for (auto [data, size] : parser.script.destinations)
    {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(data);
        borrowed |= address >= begin && address + chunk->size <= begin + size;
    }
    EXPECT_TRUE(borrowed);
    EXPECT_EQ(parser.client->ReadBody(), nullptr);
    EXPECT_TRUE(parser.client->Complete());
}

TEST(HttpClientParser, ChunkTrailersConsumedBeforeReuse)
{
    const std::string response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\nX-Checksum: abc\r\nX-Extra: " + std::string(200, 'z') +
        "\r\n\r\n" + kNextResponse;
    for (bool skip : {false, true})
    {
        SCOPED_TRACE(skip);
        Parser parser(Bytes(response), 64);
        if (skip) parser.client->SkipBody();
        else EXPECT_EQ(parser.Body(), "hello");
        EXPECT_TRUE(parser.client->Complete());
        ASSERT_TRUE(parser.client->Reusable());
        ASSERT_TRUE(parser.client->Reset());
        auto* line = parser.client->GetResponseLine();
        ASSERT_NE(line, nullptr);
        EXPECT_EQ(line->status, 201);
        ExpectReusableResponse(parser, "ok");
    }
}

TEST(HttpClientParser, BufferedNextResponseSurvivesReset)
{
    Parser parser({"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello" + kNextResponse}, 512);
    ExpectReusableResponse(parser, "hello");
    EXPECT_EQ(parser.script.reads, 1);
    ASSERT_TRUE(parser.client->Reset());
    ExpectReusableResponse(parser, "ok");
    EXPECT_EQ(parser.script.reads, 1);
}

TEST(HttpClientParser, CloseDelimitedBodyCompletesOnlyAtEof)
{
    for (const std::string& version : {"HTTP/1.0", "HTTP/1.1"})
    {
        Parser parser(Bytes(version + " 200 OK\r\n\r\nhello"), 64);
        EXPECT_EQ(parser.Body(), "hello");
        EXPECT_TRUE(parser.client->Complete());
        EXPECT_EQ(parser.client->Error(), 0);
        EXPECT_FALSE(parser.client->Reusable());
        EXPECT_EQ(parser.script.terminalReads, 1);
        const auto reads = parser.script.reads;
        EXPECT_FALSE(parser.client->Reset());
        EXPECT_TRUE(parser.client->Complete());
        EXPECT_EQ(parser.client->Error(), 0);
        EXPECT_EQ(parser.script.reads, reads);
    }
}

TEST(HttpClientParser, ConnectionCloseTokenDisablesReuse)
{
    Parser parser(Bytes("HTTP/1.1 200 OK\r\nConnection: keep-alive, CLOSE\r\n"
                        "Content-Length: 2\r\n\r\nok"));
    EXPECT_EQ(parser.Body(), "ok");
    EXPECT_TRUE(parser.client->Complete());
    EXPECT_FALSE(parser.client->Reusable());
    EXPECT_FALSE(parser.client->KeepAlive());
}

TEST(HttpClientParser, NoBodyResponsesPreserveFollowingMessage)
{
    for (int status : {204, 304})
    {
        SCOPED_TRACE(status);
        Parser parser({"HTTP/1.1 " + std::to_string(status) +
            " Empty\r\nContent-Length: 100\r\n\r\n" + kNextResponse}, 256);
        ExpectReusableResponse(parser, "");
        EXPECT_EQ(parser.client->ContentLength(), 100);
        ASSERT_TRUE(parser.client->Reset());
        ExpectReusableResponse(parser, "ok");
    }
    Parser head({"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" + kNextResponse}, 256);
    ASSERT_TRUE(head.client->Head("/"));
    ExpectReusableResponse(head, "");
    EXPECT_EQ(head.client->ContentLength(), 100);
    ASSERT_TRUE(head.client->Reset());
    ExpectReusableResponse(head, "ok");
}

TEST(HttpClientParser, TruncatedFixedLengthMessagesNeverReportCompletion)
{
    const std::string response = "HTTP/1.1 200 OK\r\nX: value\r\nContent-Length: 5\r\n\r\nhello";
    for (size_t length = 0; length < response.size(); ++length)
    {
        SCOPED_TRACE(length);
        Parser parser(Bytes(response.substr(0, length)), 64);
        parser.Body();
        EXPECT_FALSE(parser.client->Complete());
        EXPECT_LT(parser.client->Error(), 0);
        EXPECT_FALSE(parser.client->Reusable());
        EXPECT_FALSE(parser.client->KeepAlive());
        const auto reads = parser.script.reads;
        EXPECT_EQ(parser.client->ReadBody(), nullptr);
        EXPECT_EQ(parser.script.reads, reads);
    }
}

TEST(HttpClientParser, TruncatedChunkedMessagesNeverReportCompletion)
{
    const std::string response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\nTrailer: value\r\n\r\n";
    for (size_t length = 0; length < response.size(); ++length)
    {
        SCOPED_TRACE(length);
        Parser parser(Bytes(response.substr(0, length)), 64);
        parser.client->SkipBody();
        EXPECT_FALSE(parser.client->Complete());
        EXPECT_LT(parser.client->Error(), 0);
        EXPECT_FALSE(parser.client->Reusable());
    }
}

TEST(HttpClientParser, TransportErrorsArePreservedIncludingCloseDelimitedBodies)
{
    for (int error : {-ETIMEDOUT, -ECANCELED, -ECONNRESET})
    {
        for (const std::string& framing : {"Content-Length: 5\r\n", ""})
        {
            SCOPED_TRACE(error);
            SCOPED_TRACE(framing);
            Parser parser({"HTTP/1.1 200 OK\r\n" + framing + "\r\nhi"});
            parser.script.terminalResult = error;
            EXPECT_EQ(parser.Body(), "hi");
            EXPECT_FALSE(parser.client->Complete());
            EXPECT_EQ(parser.client->Error(), error);
            EXPECT_FALSE(parser.client->Reusable());
        }
    }
}

TEST(HttpClientParser, InvalidContentLengthRejectedDuringReadAndSkip)
{
    for (const std::string& value : {"", "-1", "+1", "1abc0", "1 0", "1, 2",
                                    "9223372036854775808", "18446744073709551616"})
    {
        for (bool read : {false, true})
        {
            SCOPED_TRACE(value);
            SCOPED_TRACE(read);
            Parser parser(Bytes("HTTP/1.1 200 OK\r\nContent-Length: " + value + "\r\n\r\n"));
            if (read) parser.Headers();
            parser.client->SkipBody();
            EXPECT_LT(parser.client->Error(), 0);
            EXPECT_FALSE(parser.client->Complete());
            EXPECT_FALSE(parser.client->Reusable());
        }
    }
}

TEST(HttpClientParser, IdenticalContentLengthsAreAcceptedAndConflictsRejected)
{
    Parser identical(Bytes("HTTP/1.1 200 OK\r\nContent-Length: 005\r\n"
                           "Content-Length: 5\r\n\r\nhello"));
    ExpectReusableResponse(identical, "hello");
    Parser conflict(Bytes("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                          "Content-Length: 6\r\n\r\nhello!"));
    conflict.client->SkipBody();
    EXPECT_LT(conflict.client->Error(), 0);
    EXPECT_FALSE(conflict.client->Reusable());
}

TEST(HttpClientParser, AmbiguousTransferFramingRejected)
{
    for (const std::string& headers : {
        "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n",
        "Transfer-Encoding: chunked\r\nContent-Length: 5\r\n",
        "Transfer-Encoding: chunked, chunked\r\n"})
    {
        SCOPED_TRACE(headers);
        Parser parser(Bytes("HTTP/1.1 200 OK\r\n" + headers + "\r\n0\r\n\r\n"));
        parser.client->SkipBody();
        EXPECT_LT(parser.client->Error(), 0);
        EXPECT_FALSE(parser.client->Reusable());
    }
}


TEST(HttpClientParser, TransferCodingsAreFramedWithoutDecodingPayload)
{
    Parser opaque(Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nencoded"));
    EXPECT_EQ(opaque.Body(), "encoded");
    EXPECT_TRUE(opaque.client->Complete());
    EXPECT_EQ(opaque.client->Error(), 0);
    EXPECT_FALSE(opaque.client->Reusable());
    Parser chunked(Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
                         "7\r\nencoded\r\n0\r\n\r\n"));
    ExpectReusableResponse(chunked, "encoded");
    // A token with a chunked prefix is not the chunked coding.
    //
    Parser similar(Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunkedfoo\r\n\r\nopaque"));
    EXPECT_EQ(similar.Body(), "opaque");
    EXPECT_TRUE(similar.client->Complete());
    EXPECT_FALSE(similar.client->Reusable());
}

TEST(HttpClientParser, InvalidChunkFramingRejected)
{
    for (const std::string& body : {"z\r\n", "-1\r\n", "+1\r\n", "1x\r\n",
        "10000000000000000\r\n", "1\r\naXX0\r\n\r\n", "1\na\r\n0\r\n\r\n"})
    {
        SCOPED_TRACE(body);
        Parser parser(Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + body));
        parser.client->SkipBody();
        EXPECT_LT(parser.client->Error(), 0);
        EXPECT_FALSE(parser.client->Complete());
        EXPECT_FALSE(parser.client->Reusable());
    }
}


TEST(HttpClientParser, ChunkExtensionsAcceptWhitespaceAndQuotedValues)
{
    for (const std::string& extensions : {" ; name = value", "; flag ; name=\"a,b; c\"",
        ";name=\"escaped\\\"quote\"", "\t;\tname\t=\tvalue"})
    {
        SCOPED_TRACE(extensions);
        Parser parser(Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1" +
                            extensions + "\r\nz\r\n0\r\n\r\n"), 64);
        ExpectReusableResponse(parser, "z");
    }
}

TEST(HttpClientParser, ChunkExtensionsRejectMalformedSyntax)
{
    for (const std::string& extensions : {";", ";=value", ";name=", ";;name=value",
        ";name=\"unterminated", ";name=\"closed\"junk", ";name=value stray"})
    {
        SCOPED_TRACE(extensions);
        Parser parser(Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1" +
                            extensions + "\r\nz\r\n0\r\n\r\n"), 64);
        EXPECT_FALSE(parser.client->SkipBody());
        EXPECT_EQ(parser.client->Error(), -EPROTO);
        EXPECT_FALSE(parser.client->Complete());
        EXPECT_FALSE(parser.client->Reusable());
    }
}

TEST(HttpClientParser, LargeChunkExtensionsDoNotRequireLargeReceiveBuffer)
{
    Parser parser(Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "1;long=" + std::string(300, 'a') + "\r\nz\r\n0\r\n\r\n"), 64);
    ExpectReusableResponse(parser, "z");
}


TEST(HttpClientParser, UnknownContentLengthRemainsUnknownAcrossRepeatedQueries)
{
    for (const std::string& headers : {"", "Transfer-Encoding: chunked\r\n"})
    {
        Parser parser({"HTTP/1.1 200 OK\r\n" + headers + "\r\n"});
        EXPECT_EQ(parser.client->ContentLength(), -1);
        const auto reads = parser.script.reads;
        EXPECT_EQ(parser.client->ContentLength(), -1);
        EXPECT_EQ(parser.client->Error(), 0);
        EXPECT_FALSE(parser.client->Complete());
        EXPECT_EQ(parser.script.reads, reads);
    }
}

TEST(HttpClientParser, InvalidStatusLinesRejected)
{
    for (const std::string& line : {"HTTX/1.1 200 OK", "HTTP/2.0 200 OK",
        "HTTP/1.1 20 OK", "HTTP/1.1 2000 OK", "HTTP/1.1 abc OK", "HTTP/1.1 099 OK"})
    {
        SCOPED_TRACE(line);
        Parser parser(Bytes(line + "\r\nContent-Length: 0\r\n\r\n"));
        EXPECT_EQ(parser.client->GetResponseLine(), nullptr);
        EXPECT_LT(parser.client->Error(), 0);
        EXPECT_FALSE(parser.client->Complete());
        EXPECT_FALSE(parser.client->Reusable());
    }
}

TEST(HttpClientParser, ContentLengthReportedWithoutWaitingForBody)
{
    Parser parser({"HTTP/1.1 200 OK\r\nContent-Length: 9223372036854775807\r\n\r\n"});
    EXPECT_EQ(parser.client->ContentLength(), std::numeric_limits<int64_t>::max());
    EXPECT_EQ(parser.client->Error(), 0);
    EXPECT_FALSE(parser.client->Complete());
    EXPECT_EQ(parser.script.terminalReads, 0);
}

TEST(HttpClientParser, ComposedRequestFramesBodyWithoutContentType)
{
    for (size_t sendSize : {size_t(32), size_t(512)})
    {
        Parser parser({}, 64, sendSize);
        ASSERT_TRUE(parser.client->SendRequest("POST", "/upload", nullptr, "hello", 5));
        const std::string& request = parser.script.sent;
        EXPECT_NE(request.find("POST /upload HTTP/1.1\r\n"), std::string::npos);
        EXPECT_NE(request.find("Host: example.test\r\n"), std::string::npos);
        EXPECT_NE(request.find("Content-Length: 5\r\n"), std::string::npos);
        EXPECT_EQ(request.find("Content-Type:"), std::string::npos);
        EXPECT_TRUE(request.ends_with("\r\n\r\nhello"));
    }
}


TEST(HttpClientParser, FileBodiesLargerThanFourGiBAreSentWithoutCountNarrowing)
{
    const size_t count = (size_t(1) << 32) + 123;
    const off_t offset = 17;
    Parser parser({});
    ASSERT_TRUE(parser.client->BeginRequest("PUT", "/large"));
    ASSERT_TRUE(parser.client->AppendHeader("Content-Length", count));
    ASSERT_TRUE(parser.client->EndHeaders());
    ASSERT_TRUE(parser.client->SendBodyFromFile(42, offset, count));
    EXPECT_EQ(parser.client->Error(), 0);
    EXPECT_GT(parser.script.fileSends.size(), 1);
    size_t sent = 0;
    for (auto [position, size] : parser.script.fileSends)
    {
        EXPECT_EQ(position, offset + static_cast<off_t>(sent));
        EXPECT_GT(size, 0);
        EXPECT_LE(size, static_cast<size_t>(std::numeric_limits<int>::max()));
        sent += size;
    }
    EXPECT_EQ(sent, count);
}

TEST(HttpClientParser, PartialFileSendFailureIsTerminalForEverySendingEntryPoint)
{
    Parser parser({});
    parser.script.fileFailAt = 1;
    parser.script.fileError = -EPIPE;
    EXPECT_FALSE(parser.client->SendBodyFromFile(42, 0, (size_t(1) << 32) + 123));
    EXPECT_EQ(parser.script.fileSends.size(), 2);
    EXPECT_EQ(parser.client->Error(), -EPIPE);
    const auto calls = parser.script.fileSends.size();
    EXPECT_FALSE(parser.client->SendBodyFromFile(42, 0, 1));
    EXPECT_FALSE(parser.client->SendBody("x", 1));
    EXPECT_FALSE(parser.client->AppendHeader("X", "y"));
    EXPECT_FALSE(parser.client->AppendHeader("Content-Length", size_t(1)));
    EXPECT_FALSE(parser.client->EndHeaders());
    EXPECT_FALSE(parser.client->Get("/"));
    EXPECT_FALSE(parser.client->Reset());
    EXPECT_EQ(parser.client->Error(), -EPIPE);
    EXPECT_EQ(parser.script.fileSends.size(), calls);
    EXPECT_EQ(parser.script.sends, 0);
}

TEST(HttpClientParser, FileOffsetErrorsAreDetectedBeforeAnyBufferedRequestIsSent)
{
    for (bool negative : {false, true})
    {
        Parser parser({}, 64, 512);
        ASSERT_TRUE(parser.client->BeginRequest("PUT", "/"));
        const off_t offset = negative ? -1 : std::numeric_limits<off_t>::max() - 1;
        EXPECT_FALSE(parser.client->SendBodyFromFile(42, offset, 3));
        EXPECT_EQ(parser.client->Error(), negative ? -EINVAL : -EOVERFLOW);
        EXPECT_EQ(parser.script.sends, 0);
        EXPECT_TRUE(parser.script.fileSends.empty());
        EXPECT_TRUE(parser.script.sent.empty());
    }
}

TEST(HttpClientParser, TransportSendErrorsAreTerminalWithoutFurtherIo)
{
    Parser parser({});
    parser.script.sendResult = -ECANCELED;
    EXPECT_FALSE(parser.client->SendRequest("POST", "/", nullptr, "payload", 7));
    EXPECT_EQ(parser.client->Error(), -ECANCELED);
    const auto calls = parser.script.sends;
    EXPECT_FALSE(parser.client->SendBody("x", 1));
    EXPECT_FALSE(parser.client->SendBodyFromFile(42, 0, 1));
    EXPECT_FALSE(parser.client->AppendHeader("X", "y"));
    EXPECT_FALSE(parser.client->Get("/"));
    EXPECT_FALSE(parser.client->SkipBody());
    EXPECT_EQ(parser.script.sends, calls);
    EXPECT_TRUE(parser.script.fileSends.empty());
    EXPECT_EQ(parser.script.reads, 0);
}

TEST(HttpClientParser, ComponentRequestLeavesBodyFramingUnderCallerControl)
{
    Parser parser({});
    ASSERT_TRUE(parser.client->BeginRequest("PUT", "/object"));
    ASSERT_TRUE(parser.client->AppendHeader("X-Signature", "literal-signature"));
    ASSERT_TRUE(parser.client->AppendHeader("Content-Length", size_t(5)));
    ASSERT_TRUE(parser.client->EndHeaders());
    ASSERT_TRUE(parser.client->SendBody("he", 2));
    ASSERT_TRUE(parser.client->SendBody("llo", 3));
    EXPECT_NE(parser.script.sent.find("X-Signature: literal-signature\r\n"), std::string::npos);
    EXPECT_TRUE(parser.script.sent.ends_with("\r\n\r\nhello"));
}

TEST(HttpClientParser, EmptyListMembersDoNotChangeTransferOrConnectionTokens)
{
    Parser parser(Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: ,gzip,,chunked,,\r\n"
        "Connection: , keep-alive,, close,\r\n\r\n2\r\nok\r\n0\r\n\r\n"));
    EXPECT_EQ(parser.Body(), "ok");
    EXPECT_TRUE(parser.client->Complete());
    EXPECT_EQ(parser.client->Error(), 0);
    EXPECT_FALSE(parser.client->Reusable());
}

TEST(HttpClientParser, SuccessfulConnectIgnoresFramingMetadata)
{
    Parser parser(Bytes("HTTP/1.1 200 Connection Established\r\n"
        "Content-Length: not-a-length\r\nTransfer-Encoding: not valid coding\r\n\r\n"));
    ASSERT_TRUE(parser.client->SendRequest("CONNECT", "example.test:443"));
    EXPECT_EQ(parser.Body(), "");
    EXPECT_TRUE(parser.client->Complete());
    EXPECT_EQ(parser.client->Error(), 0);
    EXPECT_EQ(parser.client->ContentLength(), -1);
    EXPECT_FALSE(parser.client->Reusable());
    EXPECT_FALSE(parser.client->Reset());
}

TEST(HttpClientParser, Http10ReuseRequiresExplicitPersistenceAndFramedBody)
{
    Parser persistent(Bytes("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n"
        "Content-Length: 2\r\n\r\nok"));
    ExpectReusableResponse(persistent, "ok");
    Parser close(Bytes("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok"));
    EXPECT_EQ(close.Body(), "ok");
    EXPECT_TRUE(close.client->Complete());
    EXPECT_FALSE(close.client->Reusable());
}

TEST(HttpClientParser, CombinedIdenticalContentLengthsAreAccepted)
{
    Parser parser(Bytes("HTTP/1.1 200 OK\r\nContent-Length: 005, 5\r\n\r\nhello"));
    ExpectReusableResponse(parser, "hello");
}

} // namespace
