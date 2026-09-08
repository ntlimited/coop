#include <gtest/gtest.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "coop/http/connection.h"
#include "coop/http/detail/server_impl.hpp"
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
    std::vector<std::pair<off_t, size_t>> fileSends;
    std::string sent;
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
        script->sent.append(static_cast<const char*>(data), size);
        return static_cast<int>(size);
    }
    int SendfileAll(int, off_t offset, size_t count)
    {
        script->fileSends.emplace_back(offset, count);
        if (script->fileError) return script->fileError;
        EXPECT_LE(count, static_cast<size_t>(INT_MAX));
        return static_cast<int>(count);
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
    ASSERT_EQ(parser.script.fileSends.size(), 2);
    EXPECT_EQ(parser.script.fileSends[0], std::make_pair(off_t(5), size_t(INT_MAX)));
    EXPECT_EQ(parser.script.fileSends[1], std::make_pair(off_t(5) + INT_MAX, size_t(4)));
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

}
