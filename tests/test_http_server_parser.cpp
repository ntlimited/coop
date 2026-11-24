#include <gtest/gtest.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

#include "coop/http/connection.h"
#include "coop/http/detail/server_impl.hpp"
#include "coop/http/status.h"
#include "coop/perf/patch.h"
#include "coop/io/uring.h"

namespace
{
struct Script
{
    std::vector<std::string> packets;
    size_t packet = 0, offset = 0, reads = 0;
    int terminal = 0;
    int sendError = 0;
    int fileError = 0;
    size_t fileLimit = SIZE_MAX;
    std::vector<std::pair<off_t, size_t>> fileSends;
    std::string sent;
    const void* lastSendData = nullptr;
};
struct Transport
{
    static constexpr bool kSpliceable = false;
    Script* script;
    coop::io::Descriptor* descriptor;
    coop::io::Descriptor& Descriptor() { return *descriptor; }
    int Recv(void* data, size_t size, int, coop::time::Interval)
    {
        ++script->reads;
        if (script->reads > 100000) throw std::runtime_error("receive loop did not terminate");
        if (script->packet == script->packets.size()) return script->terminal;
        auto& packet = script->packets[script->packet];
        size_t count = std::min(size, packet.size() - script->offset);
        if (!count) throw std::runtime_error("zero-length receive");
        memcpy(data, packet.data() + script->offset, count);
        script->offset += count;
        if (script->offset == packet.size()) { ++script->packet; script->offset = 0; }
        return static_cast<int>(count);
    }
    int SendAll(const void* data, size_t size)
    {
        if (script->sendError) return script->sendError;
        script->lastSendData = data;
        script->sent.append(static_cast<const char*>(data), size);
        return static_cast<int>(size);
    }
    int SendfileAll(int, off_t offset, size_t count)
    {
        script->fileSends.emplace_back(offset, count);
        if (script->fileError) return script->fileError;
        EXPECT_LE(count, static_cast<size_t>(INT_MAX));
        return static_cast<int>(std::min(count, script->fileLimit));
    }
    int Send(const void* data, size_t size, coop::time::Interval)
    {
        return SendAll(data, size);
    }
    int Sendfile(int fd, off_t offset, size_t count, coop::time::Interval)
    {
        return SendfileAll(fd, offset, count);
    }
};
using Connection = coop::http::Connection<Transport>;
struct Parser
{
    // The real descriptor borrows a sentinel fd from an uninitialized ring. No operation
    // initializes or enters the kernel; all parser input/output uses the static transport.
    coop::io::Uring ring;
    coop::io::Descriptor descriptor{coop::io::borrowed, -1, &ring};
    Script script;
    Connection* conn;
    explicit Parser(std::vector<std::string> packets, size_t receive = 128,
                    coop::http::ServerParserOptions options = {})
    : script{std::move(packets)}
    , conn(new (::operator new(sizeof(Connection) + Connection::ExtraBytes(receive, 64)))
        Connection(Transport{&script, &descriptor}, nullptr, nullptr, receive, 64,
                   std::chrono::seconds(30), options))
    {}
    ~Parser() { conn->~Connection(); ::operator delete(conn); }
    std::string Body()
    {
        std::string result;
        while (auto chunk = conn->NextBody())
        {
            EXPECT_EQ(chunk.Error(), 0);
            result.append(static_cast<const char*>(chunk->data), chunk->size);
        }
        return result;
    }
};
std::vector<std::string> Bytes(const std::string& input)
{
    std::vector<std::string> packets;
    for (char c : input) packets.emplace_back(1, c);
    return packets;
}
const std::string next = "GET /next HTTP/1.1\r\n\r\n";

TEST(HttpServerParser, FixedLengthAtEveryTransportSplitPreservesPipeline)
{
    const std::string wire = "POST /upload HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello" + next;
    for (size_t split = 1; split < wire.size(); ++split)
    {
        SCOPED_TRACE(split);
        Parser parser({wire.substr(0, split), wire.substr(split)});
        EXPECT_EQ(parser.Body(), "hello");
        EXPECT_TRUE(parser.conn->Complete());
        EXPECT_TRUE(parser.conn->NextBody().Complete());
        ASSERT_TRUE(parser.conn->Reset());
        ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
        EXPECT_EQ(parser.conn->GetRequestLine()->path, "/next");
    }
}
TEST(HttpServerParser, ChunkedAtEverySplitPreservesBorrowedPayloadAndPipeline)
{
    const std::string wire = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                             "3;key=\"a\\\"b\"\r\nabc\r\n2\r\nde\r\n0\r\nX-End: yes\r\n\r\n" + next;
    for (size_t split = 1; split < wire.size(); ++split)
    {
        SCOPED_TRACE(split);
        Parser parser({wire.substr(0, split), wire.substr(split)}, 64);
        EXPECT_EQ(parser.Body(), "abcde");
        EXPECT_TRUE(parser.conn->Complete());
        ASSERT_TRUE(parser.conn->Reset());
        ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
        EXPECT_EQ(parser.conn->GetRequestLine()->path, "/next");
    }
    Parser parser(Bytes(wire), 32);
    EXPECT_EQ(parser.Body(), "abcde");
    EXPECT_TRUE(parser.conn->Complete());
}
TEST(HttpServerParser, BodySpanIsReturnedBeforeReadingItsChunkDelimiter)
{
    Parser parser({"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc",
                   "\r\n0\r\n\r\n"});
    auto result = parser.conn->NextBody();
    ASSERT_TRUE(result);
    EXPECT_EQ(std::string_view(static_cast<const char*>(result->data), result->size), "abc");
    EXPECT_EQ(parser.script.reads, 1);
    EXPECT_FALSE(parser.conn->Complete());
    EXPECT_TRUE(parser.conn->NextBody().Complete());
}
TEST(HttpServerParser, EveryTruncatedRequestFailsAndCannotReset)
{
    const std::string wire = "POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc";
    for (size_t length = 1; length < wire.size(); ++length)
    {
        SCOPED_TRACE(length);
        Parser parser({wire.substr(0, length)});
        parser.Body();
        EXPECT_EQ(parser.conn->Error(), -ECONNRESET);
        EXPECT_FALSE(parser.conn->Complete());
        EXPECT_FALSE(parser.conn->Reusable());
        EXPECT_FALSE(parser.conn->SkipBody());
        EXPECT_FALSE(parser.conn->Reset());
        size_t reads = parser.script.reads;
        EXPECT_FALSE(parser.conn->NextBody());
        EXPECT_EQ(parser.script.reads, reads);
    }
}
TEST(HttpServerParser, EveryTruncatedChunkAndTrailerFails)
{
    const std::string head = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::string body = "3\r\nabc\r\n0\r\nTrailer: value\r\n\r\n";
    for (size_t length = 0; length < body.size(); ++length)
    {
        SCOPED_TRACE(length);
        Parser parser({head + body.substr(0, length)});
        parser.Body();
        EXPECT_EQ(parser.conn->Error(), -ECONNRESET);
        EXPECT_FALSE(parser.conn->Complete());
        EXPECT_FALSE(parser.conn->Reset());
    }
}
TEST(HttpServerParser, TransportErrorRemainsDistinctFromTruncation)
{
    Parser parser({"POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\na"});
    parser.script.terminal = -ETIMEDOUT;
    EXPECT_EQ(parser.Body(), "a");
    EXPECT_EQ(parser.conn->NextBody().Error(), -ETIMEDOUT);
    EXPECT_FALSE(parser.conn->KeepAlive());
    EXPECT_FALSE(parser.conn->Reset());
}
TEST(HttpServerParser, ContentLengthOverflowAndAmbiguityRejected)
{
    for (const std::string value : {"9223372036854775808", "9999999999999999999", "-1", "1a", "1,2", "1 2", "1,"})
    {
        SCOPED_TRACE(value);
        Parser parser(Bytes("POST / HTTP/1.1\r\nContent-Length: " + value + "\r\n\r\n"));
        parser.Body();
        EXPECT_NE(parser.conn->Error(), 0);
        EXPECT_EQ(parser.conn->ParseError(), 400);
        EXPECT_NE(parser.script.sent.find("400"), std::string::npos);
        EXPECT_FALSE(parser.conn->Reset());
    }
    Parser maximum({"POST / HTTP/1.1\r\nContent-Length: 9223372036854775807\r\n\r\n"});
    EXPECT_EQ(maximum.conn->ContentLength(), INT64_MAX);
    EXPECT_EQ(maximum.conn->Error(), 0);
}
TEST(HttpServerParser, RepeatedAndCommaJoinedLengthsMustAgree)
{
    Parser parser(Bytes("POST / HTTP/1.1\r\nContent-Length: 03, 3\r\nContent-Length: 3\r\n\r\nabc"));
    EXPECT_EQ(parser.Body(), "abc");
    EXPECT_TRUE(parser.conn->Reusable());
    Parser conflict({"POST / HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n"});
    conflict.Body();
    EXPECT_EQ(conflict.conn->ParseError(), 400);
}
TEST(HttpServerParser, ConnectionTokensStreamWithoutFixedScratchLimit)
{
    Parser parser(Bytes("GET / HTTP/1.1\r\nConnection: " + std::string(600, 'x') + ", ClOsE\r\n\r\n"), 32);
    EXPECT_TRUE(parser.conn->SkipBody());
    EXPECT_FALSE(parser.conn->KeepAlive());
    EXPECT_FALSE(parser.conn->Reset());
    Parser ten({"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"});
    EXPECT_TRUE(ten.conn->SkipBody());
    EXPECT_TRUE(ten.conn->Reusable());
    Parser tenClose({"GET / HTTP/1.0\r\n\r\n"});
    EXPECT_TRUE(tenClose.conn->SkipBody());
    EXPECT_FALSE(tenClose.conn->Reusable());
}
TEST(HttpServerParser, Http10ResponseVersionSurvivesBufferMovement)
{
    Parser parser(Bytes("GET / HTTP/1.0\r\nConnection: keep-alive\r\nX: " +
                        std::string(100, 'x') + "\r\n\r\n"), 32);
    ASSERT_TRUE(parser.conn->SkipBody());
    EXPECT_EQ(parser.conn->GetRequestVersion(), "HTTP/1.0");
    ASSERT_TRUE(parser.conn->BeginResponse(299, "Custom"));
    ASSERT_TRUE(parser.conn->AppendHeader("Content-Length", size_t(0)));
    ASSERT_TRUE(parser.conn->EndHeaders());
    EXPECT_EQ(parser.script.sent,
              "HTTP/1.0 299 Custom\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n");
}
TEST(HttpServerParser, TransferCodingListsAreOpaqueUntilFinalChunked)
{
    Parser parser(Bytes("POST / HTTP/1.1\r\nTransfer-Encoding: gzip; p=\"opaque\", chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n"));
    EXPECT_EQ(parser.Body(), "x");
    EXPECT_TRUE(parser.conn->Complete());
    for (const std::string value : {"gzip", "chunked, gzip", "chunked, chunked", "chunked; x=y"})
    {
        Parser bad({"POST / HTTP/1.1\r\nTransfer-Encoding: " + value + "\r\n\r\n"});
        bad.Body();
        EXPECT_EQ(bad.conn->ParseError(), 400);
    }
}
TEST(HttpServerParser, LimitsAreExplicitAndPersistAcrossReset)
{
    coop::http::ServerParserOptions options;
    options.maxHeaderCount = 0;
    Parser parser({next + "GET / HTTP/1.1\r\nX: y\r\n\r\n"}, 128, options);
    EXPECT_TRUE(parser.conn->SkipBody());
    ASSERT_TRUE(parser.conn->Reset());
    EXPECT_FALSE(parser.conn->SkipBody());
    EXPECT_EQ(parser.conn->ParseError(), 431);
    options.maxHeaderCount = SIZE_MAX;
    options.maxHeaderBytes = SIZE_MAX;
    options.maxChunkSize = SIZE_MAX;
    std::string many = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 150; ++i) many += "X: " + std::string(100, 'a') + "\r\n";
    Parser unlimited({many + "\r\n"}, 64, options);
    EXPECT_TRUE(unlimited.conn->SkipBody());
    EXPECT_FALSE(unlimited.conn->SetParserOptions({}));
}
TEST(HttpServerParser, ChunkAndTrailerBudgetsAreEnforced)
{
    coop::http::ServerParserOptions options;
    options.maxChunkSize = 2;
    Parser chunk({"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"}, 128, options);
    EXPECT_FALSE(chunk.conn->SkipBody());
    EXPECT_EQ(chunk.conn->ParseError(), 413);
    options.maxHeaderCount = 1;
    Parser trailer({"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX: y\r\n\r\n"}, 128, options);
    EXPECT_FALSE(trailer.conn->SkipBody());
    EXPECT_EQ(trailer.conn->ParseError(), 431);
}
TEST(HttpServerParser, LongExtensionsAndTrailersStreamThroughSmallBuffer)
{
    Parser parser({"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1;a=" + std::string(300, 'x') +
                   "\r\nz\r\n0\r\nLong: " + std::string(300, 'v') + "\r\n\r\n"}, 32);
    EXPECT_EQ(parser.Body(), "z");
    EXPECT_TRUE(parser.conn->Complete());
}
TEST(HttpServerParser, CallerOwnsErrorResponseButCannotResumeParsing)
{
    coop::http::ServerParserOptions options;
    options.errorResponse = coop::http::ParserErrorResponse::Caller;
    Parser parser({"POST / HTTP/1.1\r\nContent-Length: nope\r\n\r\n"}, 128, options);
    EXPECT_FALSE(parser.conn->SkipBody());
    EXPECT_TRUE(parser.script.sent.empty());
    EXPECT_EQ(parser.conn->ParseError(), 400);
    EXPECT_TRUE(parser.conn->Send(422, "text/plain", "custom", 6));
    EXPECT_NE(parser.script.sent.find("422"), std::string::npos);
    EXPECT_FALSE(parser.conn->Reset());
    EXPECT_EQ(parser.conn->NextBody().Error(), -EPROTO);
}
TEST(HttpServerParser, ResetNeverDrainsIncompleteRequest)
{
    Parser parser({"POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\n", "abc"});
    EXPECT_EQ(parser.conn->ContentLength(), 3);
    size_t reads = parser.script.reads;
    EXPECT_FALSE(parser.conn->Reset());
    EXPECT_EQ(parser.script.reads, reads);
    EXPECT_TRUE(parser.conn->SkipBody());
    EXPECT_TRUE(parser.conn->Reset());
}
TEST(HttpServerParser, SendFailureIsStickyAndPreventsReuse)
{
    Parser parser({next});
    EXPECT_TRUE(parser.conn->SkipBody());
    parser.script.sendError = -EPIPE;
    EXPECT_FALSE(parser.conn->Send(200, "text/plain", "ok", 2));
    EXPECT_EQ(parser.conn->Error(), -EPIPE);
    EXPECT_FALSE(parser.conn->Send(200, "text/plain", "again", 5));
    EXPECT_FALSE(parser.conn->Reusable());
}
TEST(HttpServerParser, MalformedRequestLineAndHeaderGrammarFail)
{
    for (const std::string wire : {"BOGUS\r\n\r\n", "BOGUS\r\nX: hello world\r\n\r\n", "GET / HTTP/9.1\r\n\r\n", "GET / HTTP/1.1\r\nBad Name: v\r\n\r\n",
                                  "GET / HTTP/1.1\r\nX: value\nwrong\r\n\r\n"})
    {
        Parser parser({wire});
        EXPECT_FALSE(parser.conn->SkipBody());
        EXPECT_EQ(parser.conn->ParseError(), 400);
    }
}
TEST(HttpServerParser, FileSendSplitsCountsAndRejectsInvalidRangesBeforeFlush)
{
    Parser parser({next});
    EXPECT_TRUE(parser.conn->Sendfile(123, 0, 0));
    EXPECT_TRUE(parser.script.fileSends.empty());
    const size_t count = static_cast<size_t>(INT_MAX) + 4;
    ASSERT_TRUE(parser.conn->Sendfile(123, 5, count));
    constexpr size_t maxWrite = 16384;
    const size_t calls = (count + maxWrite - 1) / maxWrite;
    ASSERT_EQ(parser.script.fileSends.size(), calls);
    EXPECT_EQ(parser.script.fileSends.front(), std::make_pair(off_t(5), maxWrite));
    EXPECT_EQ(parser.script.fileSends.back(),
              std::make_pair(off_t(5 + (calls - 1) * maxWrite), count % maxWrite));
    Parser bad({next});
    ASSERT_TRUE(bad.conn->BeginResponse(200));
    EXPECT_FALSE(bad.conn->Sendfile(123, std::numeric_limits<off_t>::max(), 1));
    EXPECT_EQ(bad.conn->Error(), -EOVERFLOW);
    EXPECT_EQ(bad.conn->ParseError(), 0);
    EXPECT_TRUE(bad.script.sent.empty());
    EXPECT_TRUE(bad.script.fileSends.empty());
    Parser failed({next});
    failed.script.fileError = -ENOSPC;
    EXPECT_FALSE(failed.conn->Sendfile(123, 0, 10));
    EXPECT_EQ(failed.conn->Error(), -ENOSPC);
    EXPECT_TRUE(failed.conn->SendError());
    EXPECT_EQ(failed.conn->ParseError(), 0);
}
TEST(HttpServerParser, QueryFlagDoesNotTurnHttpVersionIntoAnotherArgument)
{
    Parser parser({"GET /?value=x&flag HTTP/1.1\r\nX: y\r\n\r\n"});
    ASSERT_STREQ(parser.conn->NextArgName(), "value");
    auto* value = parser.conn->ReadArgValue();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(std::string_view(static_cast<const char*>(value->data), value->size), "x");
    ASSERT_STREQ(parser.conn->NextArgName(), "flag");
    EXPECT_EQ(parser.conn->NextArgName(), nullptr);
    ASSERT_STREQ(parser.conn->NextHeaderName(), "X");
    EXPECT_TRUE(parser.conn->SkipBody());
}
TEST(HttpServerParser, HeaderByteLimitCountsWireFieldsExactly)
{
    coop::http::ServerParserOptions options;
    options.maxHeaderBytes = 6; // X: y\r\n
    Parser fits(Bytes("GET / HTTP/1.1\r\nX: y\r\n\r\n"), 32, options);
    EXPECT_TRUE(fits.conn->SkipBody());
    options.maxHeaderBytes = 5;
    Parser fails(Bytes("GET / HTTP/1.1\r\nX: y\r\n\r\n"), 32, options);
    EXPECT_FALSE(fails.conn->SkipBody());
    EXPECT_EQ(fails.conn->ParseError(), 431);
}

TEST(HttpServerParser, EmptyTransferEncodingFailsWithoutConsumingPipeline)
{
    for (const std::string value : {"", " \t ", ", ,\t,"})
    {
        for (int contentLength = 0; contentLength != 3; ++contentLength)
        {
            const std::string length = "Content-Length: 0\r\n";
            const std::string wire = "POST / HTTP/1.1\r\n" +
                (contentLength == 1 ? length : "") + "Transfer-Encoding:" + value + "\r\n" +
                (contentLength == 2 ? length : "") + "\r\n" + next;
            SCOPED_TRACE(wire);
            for (auto response : {coop::http::ParserErrorResponse::Automatic,
                                  coop::http::ParserErrorResponse::Caller})
            {
                coop::http::ServerParserOptions options;
                options.errorResponse = response;
                // Exercise whole input, byte fragments, and every two-packet boundary.
                for (size_t split = 0; split <= wire.size(); ++split)
                {
                    SCOPED_TRACE(split);
                    auto packets = split == 0 ? Bytes(wire) :
                        split == wire.size() ? std::vector<std::string>{wire} :
                        std::vector<std::string>{wire.substr(0, split), wire.substr(split)};
                    Parser parser(std::move(packets), 64, options);
                    EXPECT_FALSE(parser.conn->SkipBody());
                    EXPECT_EQ(parser.conn->Error(), -EPROTO);
                    EXPECT_EQ(parser.conn->ParseError(), 400);
                    EXPECT_FALSE(parser.conn->Complete());
                    EXPECT_FALSE(parser.conn->Reusable());
                    EXPECT_FALSE(parser.conn->KeepAlive());
                    const size_t reads = parser.script.reads;
                    const std::string sent = parser.script.sent;
                    EXPECT_FALSE(parser.conn->Reset());
                    EXPECT_FALSE(parser.conn->SkipBody());
                    EXPECT_EQ(parser.conn->NextBody().Error(), -EPROTO);
                    EXPECT_EQ(parser.script.reads, reads);
                    EXPECT_EQ(parser.script.sent, sent);
                    if (response == coop::http::ParserErrorResponse::Automatic)
                        EXPECT_NE(sent.find("400"), std::string::npos);
                    else
                        EXPECT_TRUE(sent.empty());
                }
            }
        }
    }
}
TEST(HttpServerParser, EmptyListMembersPreserveValidTransferCodingsAndReset)
{
    for (const std::string fields : {
        "Transfer-Encoding: , gzip; p=\"opaque\", , chunked, \r\n",
        "Transfer-Encoding: , \r\nTransfer-Encoding: chunked\r\n",
        "Transfer-Encoding: chunked\r\nTransfer-Encoding: , \r\n"})
    {
        SCOPED_TRACE(fields);
        const std::string wire = "POST / HTTP/1.1\r\n" + fields + "\r\n1\r\nx\r\n0\r\n\r\n" +
            "POST /next HTTP/1.1\r\nContent-Length: 1\r\n\r\ny";
        for (auto packets : {std::vector<std::string>{wire}, Bytes(wire)})
        {
            Parser parser(std::move(packets), 64);
            EXPECT_EQ(parser.Body(), "x");
            ASSERT_TRUE(parser.conn->Reusable());
            ASSERT_TRUE(parser.conn->Reset());
            EXPECT_EQ(parser.Body(), "y");
            EXPECT_TRUE(parser.conn->Reusable());
        }
    }
}

TEST(HttpServerParser, ContentLengthQueriesPreserveAbsentZeroAndChunkedFraming)
{
    for (const std::string fields : {"", "Content-Length: 0\r\n",
                                     "Transfer-Encoding: chunked\r\n"})
    {
        SCOPED_TRACE(fields);
        const bool chunked = fields.find("chunked") != std::string::npos;
        const int64_t expected = fields == "Content-Length: 0\r\n" ? 0 : -1;
        const std::string body = chunked ? "1\r\nx\r\n0\r\n\r\n" : "";
        const std::string wire = "POST / HTTP/1.1\r\n" + fields + "\r\n" + body + next;
        for (size_t split = 1; split < wire.size(); ++split)
        {
            SCOPED_TRACE(split);
            Parser parser({wire.substr(0, split), wire.substr(split)});
            EXPECT_EQ(parser.conn->ContentLength(), expected);
            const size_t reads = parser.script.reads;
            EXPECT_EQ(parser.conn->ContentLength(), expected);
            EXPECT_EQ(parser.script.reads, reads);
            EXPECT_EQ(parser.conn->Error(), 0);
            EXPECT_EQ(parser.Body(), chunked ? "x" : "");
            ASSERT_TRUE(parser.conn->Reset());
            EXPECT_EQ(parser.conn->ContentLength(), -1);
            EXPECT_TRUE(parser.conn->Complete());
        }
    }
}

TEST(HttpServerParser, ContentLengthQueryValidatesAllHeadersAfterPartialIteration)
{
    for (const std::string later : {"Content-Length: 1\r\n", "Content-Length: nope\r\n",
                                    "Transfer-Encoding: chunked\r\n"})
    {
        const std::string wire = "POST / HTTP/1.1\r\nContent-Length: 0\r\n" + later + "\r\n";
        for (size_t split = 1; split < wire.size(); ++split)
        {
            SCOPED_TRACE(later);
            SCOPED_TRACE(split);
            Parser parser({wire.substr(0, split), wire.substr(split), next});
            ASSERT_STREQ(parser.conn->NextHeaderName(), "Content-Length");
            parser.conn->SkipHeaderValue();
            EXPECT_EQ(parser.conn->ContentLength(), -1);
            EXPECT_EQ(parser.conn->ParseError(), 400);
            EXPECT_LT(parser.conn->Error(), 0);
            EXPECT_FALSE(parser.conn->Complete());
            EXPECT_FALSE(parser.conn->Reusable());
            const size_t reads = parser.script.reads;
            EXPECT_EQ(parser.conn->ContentLength(), -1);
            EXPECT_FALSE(parser.conn->Reset());
            EXPECT_EQ(parser.script.reads, reads);
        }
    }
}

}

TEST(HttpServerParser, FileSendAdvancesByPartialProgress)
{
    Parser parser({next});
    parser.script.fileLimit = 7;
    ASSERT_TRUE(parser.conn->Sendfile(123, 5, 100));
    ASSERT_EQ(parser.script.fileSends.size(), 15u);
    EXPECT_EQ(parser.script.fileSends[1].first, 12);
    EXPECT_EQ(parser.script.fileSends.back(), std::make_pair(off_t(103), size_t(2)));
}

TEST(HttpServerParser, StatusDispatchRejectsBodyBeforeSamplerResponse)
{
    Parser parser({"GET /api/sampler HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
                   "invalid chunk\r\n"});
    ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
    EXPECT_TRUE(coop::http::StatusDispatch(*parser.conn, "/api/sampler"));
    EXPECT_NE(parser.script.sent.find("400 Bad Request\r\n"), std::string::npos);
    EXPECT_EQ(parser.script.sent.find("200 OK\r\n"), std::string::npos);
    EXPECT_NE(parser.script.sent.find("Connection: close\r\n"), std::string::npos);
    EXPECT_EQ(parser.script.reads, 1);
    EXPECT_FALSE(parser.conn->KeepAlive());
    EXPECT_FALSE(parser.conn->Reset());
}

TEST(HttpServerParser, NoBodyGuardPrecedesHandlerAndNeverReadsBody)
{
    for (const char* framing : {"Content-Length: 1\r\n", "Transfer-Encoding: chunked\r\n",
                                "Transfer-Encoding: gzip, chunked\r\n"})
    {
        for (auto policy : {coop::http::ParserErrorResponse::Automatic,
                            coop::http::ParserErrorResponse::Caller})
        {
            SCOPED_TRACE(framing);
            coop::http::ServerParserOptions options;
            options.errorResponse = policy;
            std::string head = "GET /no-body HTTP/1.1\r\n" + std::string(framing) + "\r\n";
            Parser parser({head, "invalid chunk\r\n"}, 128, options);
            bool handlerRan = false;
            auto dispatch = [&]
            {
                if (!parser.conn->RequireNoBody()) return;
                handlerRan = true;
                parser.conn->Send(200, "text/plain", "ok");
            };
            dispatch();
            EXPECT_FALSE(handlerRan);
            EXPECT_EQ(parser.script.reads, 1);
            EXPECT_EQ(parser.conn->ParseError(), 400);
            EXPECT_EQ(parser.conn->Error(), -EPROTO);
            EXPECT_FALSE(parser.conn->KeepAlive());
            EXPECT_FALSE(parser.conn->SkipBody());
            EXPECT_FALSE(parser.conn->Reset());
            EXPECT_NE(parser.script.sent.find("400 Bad Request\r\n"), std::string::npos);
            EXPECT_NE(parser.script.sent.find("Connection: close\r\n"), std::string::npos);
            std::string response = parser.script.sent;
            EXPECT_FALSE(parser.conn->RequireNoBody());
            EXPECT_FALSE(parser.conn->Send(200, "text/plain", "late"));
            EXPECT_EQ(parser.script.sent, response);
        }
    }
}

TEST(HttpServerParser, NoBodyGuardAcceptsAbsentAndZeroLengthAndPreservesPipeline)
{
    for (const char* framing : {"", "Content-Length: 0\r\n"})
    {
        std::string request = "GET /no-body HTTP/1.1\r\n" + std::string(framing) + "\r\n";
        for (size_t split = 1; split < request.size(); ++split)
        {
            SCOPED_TRACE(split);
            Parser parser({request.substr(0, split), request.substr(split) + next});
            EXPECT_TRUE(parser.conn->RequireNoBody());
            EXPECT_TRUE(parser.conn->Complete());
            EXPECT_TRUE(parser.conn->KeepAlive());
            EXPECT_TRUE(parser.script.sent.empty());
            ASSERT_TRUE(parser.conn->Reset());
            ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
            EXPECT_EQ(parser.conn->GetRequestLine()->path, "/next");
        }
    }
}

TEST(HttpServerParser, AllNoBodyStatusRoutesRejectFramingBeforeTheirHandler)
{
    for (const char* path : {"/api/status", "/api/perf", "/api/perf/enable",
            "/api/perf/disable", "/api/sampler", "/api/sampler/start", "/api/sampler/stop",
            "/api/sampler/samples", "/api/sampler/folded", "/api/health", "/api/cooperators",
            "/api/cooperators/perf", "/api/epoch", "/api/epoch/all", "/api/contexts/stacks",
            "/api/contexts/dump"})
    {
        SCOPED_TRACE(path);
        Parser parser({"GET " + std::string(path) +
                       "?families=io&hz=17&stacks=1 HTTP/1.1\r\nContent-Length: 1\r\n\r\n",
                       "x"});
        ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
        EXPECT_TRUE(coop::http::StatusDispatch(*parser.conn, path));
        EXPECT_NE(parser.script.sent.find("400 Bad Request\r\n"), std::string::npos);
        EXPECT_EQ(parser.script.sent.find("200 OK\r\n"), std::string::npos);
        EXPECT_EQ(parser.script.reads, 1);
    }
}

TEST(HttpServerParser, StatusQueryOptionsSurviveNoBodyValidation)
{
    auto original = coop::perf::EnabledFamilies();
    coop::perf::SetFamilies({});
    Parser enable({"GET /api/perf/enable?families=io HTTP/1.1\r\n\r\n"});
    ASSERT_NE(enable.conn->GetRequestLine(), nullptr);
    EXPECT_TRUE(coop::http::StatusDispatch(*enable.conn, "/api/perf/enable"));
    EXPECT_NE(enable.script.sent.find("200 OK\r\n"), std::string::npos);
#if COOP_PERF_MODE == 2
    EXPECT_EQ(coop::perf::EnabledFamilies(), coop::perf::Family::IO);
#endif
    Parser disable({"GET /api/perf/disable?families=io HTTP/1.1\r\n\r\n"});
    ASSERT_NE(disable.conn->GetRequestLine(), nullptr);
    EXPECT_TRUE(coop::http::StatusDispatch(*disable.conn, "/api/perf/disable"));
    EXPECT_NE(disable.script.sent.find("200 OK\r\n"), std::string::npos);
#if COOP_PERF_MODE == 2
    EXPECT_EQ(coop::perf::EnabledFamilies(), coop::perf::Family{});
#endif
    coop::perf::SetFamilies(original);
}

TEST(HttpServerParser, SymbolizeStatusRouteRetainsItsRequestBody)
{
    Parser parser({"POST /api/sampler/symbolize HTTP/1.1\r\nContent-Length: 3\r\n\r\n0x0"});
    ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
    EXPECT_TRUE(coop::http::StatusDispatch(*parser.conn, "/api/sampler/symbolize"));
    EXPECT_NE(parser.script.sent.find("200 OK\r\n"), std::string::npos);
    EXPECT_TRUE(parser.conn->Complete());
}

TEST(HttpServerParser, NoBodyGuardOwnsNewHeaderFailuresAndRestoresCallerPolicy)
{
    for (const char* framing : {"Transfer-Encoding:\r\n", "Transfer-Encoding: gzip\r\n",
            "Transfer-Encoding: chunked;invalid\r\n", "Transfer-Encoding: chunked, gzip\r\n",
            "Content-Length: invalid\r\n"})
    {
        SCOPED_TRACE(framing);
        coop::http::ServerParserOptions options;
        options.errorResponse = coop::http::ParserErrorResponse::Caller;
        Parser parser({"GET /no-body HTTP/1.1\r\n" + std::string(framing) + "\r\n",
                       "invalid chunk\r\n"}, 128, options);
        bool handlerRan = false;
        if (parser.conn->RequireNoBody()) handlerRan = true;
        EXPECT_FALSE(handlerRan);
        EXPECT_EQ(parser.script.reads, 1);
        EXPECT_EQ(parser.conn->ParseError(), 400);
        EXPECT_EQ(parser.conn->Error(), -EPROTO);
        EXPECT_EQ(parser.conn->GetParserOptions().errorResponse, options.errorResponse);
        EXPECT_FALSE(parser.conn->KeepAlive());
        EXPECT_FALSE(parser.conn->Reset());
        EXPECT_NE(parser.script.sent.find("400 Bad Request\r\n"), std::string::npos);
        EXPECT_NE(parser.script.sent.find("Connection: close\r\n"), std::string::npos);
        std::string response = parser.script.sent;
        EXPECT_FALSE(parser.conn->RequireNoBody());
        EXPECT_FALSE(parser.conn->Send(200, "text/plain", "late"));
        EXPECT_EQ(parser.script.sent, response);
    }

    coop::http::ServerParserOptions options;
    options.errorResponse = coop::http::ParserErrorResponse::Caller;
    options.maxHeaderCount = 0;
    Parser limited({"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"}, 128, options);
    EXPECT_FALSE(limited.conn->RequireNoBody());
    EXPECT_EQ(limited.conn->ParseError(), 431);
    EXPECT_NE(limited.script.sent.find("431 Request Header Fields Too Large\r\n"),
              std::string::npos);
    EXPECT_EQ(limited.conn->GetParserOptions().errorResponse, options.errorResponse);
}

TEST(HttpServerParser, NoBodyGuardPreservesExistingFailureAndAvoidsSecondResponse)
{
    coop::http::ServerParserOptions options;
    options.errorResponse = coop::http::ParserErrorResponse::Caller;
    Parser failed({"GET / HTTP/1.1\r\nTransfer-Encoding:\r\n\r\n"}, 128, options);
    failed.conn->SkipHeaders();
    ASSERT_EQ(failed.conn->ParseError(), 400);
    EXPECT_TRUE(failed.script.sent.empty());
    EXPECT_FALSE(failed.conn->RequireNoBody());
    EXPECT_TRUE(failed.script.sent.empty());
    EXPECT_EQ(failed.conn->GetParserOptions().errorResponse, options.errorResponse);

    for (const char* framing : {"Transfer-Encoding: chunked\r\n", "Transfer-Encoding:\r\n"})
    {
        SCOPED_TRACE(framing);
        Parser started({"GET / HTTP/1.1\r\n" + std::string(framing) + "\r\n"}, 128, options);
        ASSERT_NE(started.conn->GetRequestLine(), nullptr);
        ASSERT_TRUE(started.conn->Send(200, "text/plain", "started"));
        std::string response = started.script.sent;
        EXPECT_FALSE(started.conn->RequireNoBody());
        EXPECT_EQ(started.conn->ParseError(), 400);
        EXPECT_FALSE(started.conn->KeepAlive());
        EXPECT_FALSE(started.conn->Send(200, "text/plain", "late"));
        EXPECT_EQ(started.script.sent, response);
    }
}

TEST(HttpServerParser, LegacyNoBodyAdmissionDefaultsToRefusalWithoutReading)
{
    Parser parser({"GET / HTTP/1.1\r\n\r\n"});
    EXPECT_FALSE(parser.conn->coop::http::ConnectionBase::RequireNoBody());
    EXPECT_FALSE(parser.conn->KeepAlive());
    EXPECT_EQ(parser.script.reads, 0);
    EXPECT_TRUE(parser.script.sent.empty());
}

TEST(HttpServerParser, Http10FixedLengthKeepAliveRetainsPersistence)
{
    Parser parser({"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n" + next});
    EXPECT_TRUE(parser.conn->GetRequestVersion().empty());
    ASSERT_TRUE(parser.conn->SkipBody());
    EXPECT_EQ(parser.conn->GetRequestVersion(), "HTTP/1.0");
    ASSERT_TRUE(parser.conn->Send(200, "text/plain", "hello"));
    EXPECT_NE(parser.script.sent.find("HTTP/1.0 200 OK\r\n"), std::string::npos);
    EXPECT_NE(parser.script.sent.find("Content-Length: 5\r\n"), std::string::npos);
    EXPECT_NE(parser.script.sent.find("Connection: keep-alive\r\n"), std::string::npos);
    EXPECT_TRUE(parser.conn->Reusable());
    ASSERT_TRUE(parser.conn->Reset());
    EXPECT_TRUE(parser.conn->GetRequestVersion().empty());
    ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
    EXPECT_EQ(parser.conn->GetRequestLine()->path, "/next");
    EXPECT_EQ(parser.conn->GetRequestVersion(), "HTTP/1.1");
}

TEST(HttpServerParser, Http10DisallowedHeadersAndInformationalResponsesAreSticky)
{
    for (const char* name : {"Transfer-Encoding", "Trailer"})
    {
        for (bool numeric : {false, true})
        {
            Parser parser({"GET / HTTP/1.0\r\n\r\n"});
            ASSERT_TRUE(parser.conn->SkipBody());
            ASSERT_TRUE(parser.conn->BeginResponse(200));
            EXPECT_FALSE(numeric ? parser.conn->AppendHeader(name, size_t(1))
                                 : parser.conn->AppendHeader(name, "chunked"));
            EXPECT_TRUE(parser.conn->SendError());
            EXPECT_FALSE(parser.conn->SendRawBytes("corrupt", 7));
            EXPECT_FALSE(parser.conn->EndHeaders());
            EXPECT_TRUE(parser.script.sent.empty());
        }
    }
    for (int status : {100, 101, 103, 199})
    {
        Parser parser({"GET / HTTP/1.0\r\n\r\n"});
        ASSERT_TRUE(parser.conn->SkipBody());
        EXPECT_FALSE(parser.conn->BeginResponse(status));
        EXPECT_TRUE(parser.conn->SendError());
        EXPECT_FALSE(parser.conn->Send(200, "text/plain", "late"));
        EXPECT_TRUE(parser.script.sent.empty());
    }
}

TEST(HttpServerParser, Http10ComponentResponseWithoutLengthCloses)
{
    Parser parser({"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"});
    ASSERT_TRUE(parser.conn->SkipBody());
    ASSERT_TRUE(parser.conn->BeginResponse(200));
    ASSERT_TRUE(parser.conn->EndHeaders());
    EXPECT_NE(parser.script.sent.find("Connection: close\r\n"), std::string::npos);
    EXPECT_FALSE(parser.conn->Reusable());
}

TEST(HttpServerParser, Http10EarlyFixedResponseChoosesClosureWithoutConsumingHeaders)
{
    Parser parser({"GET /?needed=value HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"});
    ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
    ASSERT_TRUE(parser.conn->Send(200, "text/plain", "hello"));
    EXPECT_NE(parser.script.sent.find("Connection: close\r\n"), std::string::npos);
    ASSERT_STREQ(parser.conn->NextArgName(), "needed");
    auto value = parser.conn->ReadArgValue();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(std::string_view(static_cast<const char*>(value->data), value->size), "value");
    EXPECT_TRUE(parser.conn->SkipBody());
    EXPECT_FALSE(parser.conn->KeepAlive());
    EXPECT_FALSE(parser.conn->Reset());
}

TEST(HttpServerParser, SendBorrowsAnExactTextView)
{
    Parser parser({"GET / HTTP/1.1\r\n\r\n"});
    const char text[] = {'x', 'a', '\0', 'b', 'z'};
    coop::http::ConnectionBase& conn = *parser.conn;
    ASSERT_TRUE(conn.Send(200, "text/plain", std::string_view(text + 1, 3)));
    EXPECT_NE(parser.script.sent.find("Content-Length: 3\r\n"), std::string::npos);
    EXPECT_EQ(parser.script.sent.substr(parser.script.sent.find("\r\n\r\n") + 4),
              std::string(text + 1, 3));
}

TEST(HttpServerParser, SendBorrowsBinaryStorageWithoutAnOwningAdapter)
{
    Parser parser({"GET / HTTP/1.1\r\n\r\n"});
    std::byte body[128]{};
    body[0] = std::byte{0xff};
    body[127] = std::byte{0x7f};
    ASSERT_TRUE(parser.conn->Send(200, "application/octet-stream",
                                 std::span<const std::byte>(body)));
    EXPECT_EQ(parser.script.lastSendData, body);
    EXPECT_NE(parser.script.sent.find("Content-Length: 128\r\n"), std::string::npos);
    EXPECT_EQ(parser.script.sent.substr(parser.script.sent.find("\r\n\r\n") + 4),
              std::string(reinterpret_cast<const char*>(body), sizeof(body)));
}

namespace
{
template<typename Body>
concept CanSendBorrowedBody = requires(coop::http::ConnectionBase& conn, Body&& body)
{
    conn.Send(200, "text/plain", static_cast<Body&&>(body));
};
static_assert(!CanSendBorrowedBody<std::nullptr_t>);
static_assert(CanSendBorrowedBody<std::string_view>);
static_assert(CanSendBorrowedBody<std::string&>);
}

TEST(HttpServerParser, EmptyBracesSendAnEmptyBody)
{
    Parser parser({"GET / HTTP/1.1\r\nHost: example.test\r\n\r\n"});
    ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
    ASSERT_TRUE(parser.conn->Send(200, "text/plain", {}));
    EXPECT_NE(parser.script.sent.find("Content-Length: 0\r\n"), std::string::npos);
    EXPECT_TRUE(parser.script.sent.ends_with("\r\n\r\n"));
}

TEST(HttpServerParser, NullTextPointerIsRefusedWithoutReadingOrSending)
{
    Parser parser({"GET / HTTP/1.1\r\nHost: example.test\r\n\r\n"});
    ASSERT_NE(parser.conn->GetRequestLine(), nullptr);
    const char* text = nullptr;
    EXPECT_FALSE(parser.conn->Send(200, "text/plain", text));
    EXPECT_TRUE(parser.script.sent.empty());
    EXPECT_FALSE(parser.conn->SendError());
    EXPECT_TRUE(parser.conn->Send(200, "text/plain", "ok"));
}
