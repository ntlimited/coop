#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "coop/io/detail/dns.hpp"

namespace
{
using namespace coop::io;
using namespace coop::io::detail;

std::vector<uint8_t> Query(std::string_view name = "www.example.test")
{
    std::vector<uint8_t> query(512);
    int n = DnsQuery(name, query, 0x1234);
    EXPECT_GT(n, 0);
    query.resize(n > 0 ? n : 0);
    return query;
}

std::vector<uint8_t> Reply(const std::vector<uint8_t>& query)
{
    auto packet = query;
    packet[2] = 0x81;
    packet[3] = 0x80;
    return packet;
}

void U16(std::vector<uint8_t>& p, unsigned n)
{
    p.push_back(n >> 8);
    p.push_back(n & 255);
}

std::vector<uint8_t> Name(std::string_view name)
{
    auto query = Query(name);
    return {query.begin() + 12, query.end() - 4};
}

void Record(std::vector<uint8_t>& p, std::vector<uint8_t> owner,
            unsigned type, std::vector<uint8_t> data, unsigned klass = 1)
{
    p.insert(p.end(), owner.begin(), owner.end());
    U16(p, type); U16(p, klass);
    p.insert(p.end(), 4, 0);
    U16(p, data.size());
    p.insert(p.end(), data.begin(), data.end());
    ++p[7];
}

TEST(DnsParserTest, RejectsCompressionCycles)
{
    auto query = Query();
    auto packet = Reply(query);
    packet.resize(18);
    packet[12] = 0xc0; packet[13] = 12;
    packet[14] = 0; packet[15] = 1; packet[16] = 0; packet[17] = 1;
    in_addr result{};
    EXPECT_EQ(DnsResponse(packet, query, result), -EPROTO);
    // A cycle containing a literal label must also make bounded progress.
    std::array<uint8_t, 4> labels = {1, 'a', 0xc0, 0};
    size_t end;
    EXPECT_EQ((DnsName{labels, 0}.End(end)), -EPROTO);
}

TEST(DnsParserTest, HostsCrLfAliasesCaseAndComments)
{
    in_addr result{};
    EXPECT_EQ(DnsHosts("127.0.0.1 localhost ALIAS\r\n192.0.2.1 other#ignore", "alias.", result), 0);
    EXPECT_EQ(result.s_addr, htonl(0x7f000001));
    EXPECT_EQ(DnsHosts("127.0.0.1 localhost\r\n", "missing", result), -ENOENT);
    EXPECT_EQ(DnsHosts("::1 localhost\r\n192.0.2.1 other#ignore", "other", result), 0);
    EXPECT_EQ(result.s_addr, htonl(0xc0000201));
}

TEST(DnsParserTest, AcceptsOnlyAssociatedAddress)
{
    auto query = Query();
    auto packet = Reply(query);
    Record(packet, Name("unrelated.test"), 1, {1, 2, 3, 4});
    Record(packet, {0xc0, 12}, 1, {192, 0, 2, 9});
    in_addr result{};
    EXPECT_EQ(DnsResponse(packet, query, result), 0);
    EXPECT_EQ(result.s_addr, htonl(0xc0000209));
}

TEST(DnsParserTest, ChecksQuestionBeforeNegativeAnswer)
{
    auto query = Query();
    auto packet = Reply(Query("bad.example.test"));
    packet[3] |= 3;
    in_addr result{};
    EXPECT_EQ(DnsResponse(packet, query, result), -EPROTO);
    packet = Reply(query);
    packet[3] |= 3;
    EXPECT_EQ(DnsResponse(packet, query, result), -ENOENT);
}

TEST(DnsParserTest, TruncationIsAnExplicitFallbackDecision)
{
    auto query = Query();
    auto packet = Reply(query);
    packet[2] |= 2;
    packet[7] = 5; // A truncated packet need not contain its claimed records.
    in_addr result{};
    EXPECT_EQ(DnsResponse(packet, query, result), -EMSGSIZE);
}


TEST(DnsParserTest, FollowsOnlyAssociatedCnameChains)
{
    auto query = Query();
    auto packet = Reply(query);
    // Put target A before the alias and an unrelated A before both.
    Record(packet, Name("unrelated.test"), 1, {1, 2, 3, 4});
    Record(packet, Name("target.test"), 1, {192, 0, 2, 10});
    Record(packet, {0xc0, 12}, 5, Name("TARGET.test"));
    in_addr result{};
    EXPECT_EQ(DnsResponse(packet, query, result), 0);
    EXPECT_EQ(result.s_addr, htonl(0xc000020a));
}

TEST(DnsParserTest, RejectsCnameLoopsAndConflictingOwners)
{
    auto query = Query();
    auto packet = Reply(query);
    Record(packet, {0xc0, 12}, 5, {0xc0, 12});
    in_addr result{};
    EXPECT_EQ(DnsResponse(packet, query, result), -EPROTO);
    packet = Reply(query);
    Record(packet, {0xc0, 12}, 5, Name("target.test"));
    Record(packet, {0xc0, 12}, 1, {1, 2, 3, 4});
    EXPECT_EQ(DnsResponse(packet, query, result), -EPROTO);
    packet = Reply(query);
    Record(packet, {0xc0, 12}, 5, Name("target.test"));
    Record(packet, {0xc0, 12}, 5, Name("other.test"));
    EXPECT_EQ(DnsResponse(packet, query, result), -EPROTO);
}

TEST(DnsParserTest, CnameWithoutAddressAndUnrelatedAnswersAreNoData)
{
    auto query = Query();
    auto packet = Reply(query);
    Record(packet, {0xc0, 12}, 5, Name("target.test"));
    Record(packet, Name("other.test"), 1, {1, 2, 3, 4});
    in_addr result{htonl(42)};
    EXPECT_EQ(DnsResponse(packet, query, result), -ENODATA);
    EXPECT_EQ(result.s_addr, htonl(42));
    packet = Reply(query);
    Record(packet, Name("target.test"), 1, {1, 2, 3, 4});
    EXPECT_EQ(DnsResponse(packet, query, result), -ENODATA);
}

TEST(DnsParserTest, AdditionalAddressesAreNotAnswers)
{
    auto query = Query();
    auto packet = Reply(query);
    Record(packet, {0xc0, 12}, 1, {1, 2, 3, 4});
    packet[7] = 0;
    packet[11] = 1;
    in_addr result{};
    EXPECT_EQ(DnsResponse(packet, query, result), -ENODATA);
}

TEST(DnsParserTest, ValidatesEveryRecordBeforePublishingSuccess)
{
    auto query = Query();
    auto packet = Reply(query);
    Record(packet, {0xc0, 12}, 1, {1, 2, 3, 4});
    Record(packet, Name("other.test"), 1, {1, 2, 3});
    in_addr result{htonl(42)};
    EXPECT_EQ(DnsResponse(packet, query, result), -EPROTO);
    EXPECT_EQ(result.s_addr, htonl(42));
    packet = Reply(query);
    Record(packet, {0xc0, 12}, 1, {1, 2, 3, 4});
    // Declared-but-missing authority record must not launder a short response into success.
    packet[9] = 1;
    EXPECT_EQ(DnsResponse(packet, query, result), -EPROTO);
    EXPECT_EQ(result.s_addr, htonl(42));
}

TEST(DnsParserTest, RejectsEveryTruncatedPrefix)
{
    auto query = Query();
    auto packet = Reply(query);
    Record(packet, {0xc0, 12}, 1, {1, 2, 3, 4});
    in_addr result{};
    for (size_t n = 0; n < packet.size(); ++n)
        EXPECT_EQ(DnsResponse({packet.data(), n}, query, result), -EPROTO) << n;
    EXPECT_EQ(DnsResponse(packet, query, result), 0);
    packet.push_back(0);
    EXPECT_EQ(DnsResponse(packet, query, result), -EPROTO);
}

TEST(DnsParserTest, ChecksIdOpcodeQuestionCountTypeAndClass)
{
    auto query = Query();
    auto packet = Reply(query);
    in_addr result{};
    for (size_t offset : {size_t(0), size_t(2), size_t(5), packet.size() - 1, packet.size() - 3})
    {
        auto malformed = packet;
        malformed[offset] ^= offset == 2 ? 8 : 1;
        EXPECT_EQ(DnsResponse(malformed, query, result), -EPROTO) << offset;
    }
    auto mixedCase = Reply(Query("WWW.ExAmPlE.TeSt"));
    Record(mixedCase, {0xc0, 12}, 1, {192, 0, 2, 11});
    EXPECT_EQ(DnsResponse(mixedCase, query, result), 0);
}

TEST(DnsParserTest, ServerFailuresKeepTheirMeaning)
{
    auto query = Query();
    in_addr result{};
    for (auto [rcode, error] : {std::pair{2, -EAGAIN}, {3, -ENOENT}, {5, -EACCES}, {1, -EPROTO}})
    {
        auto packet = Reply(query);
        packet[3] |= rcode;
        EXPECT_EQ(DnsResponse(packet, query, result), error);
    }
}

TEST(DnsParserTest, EnforcesProtocolNameLimitsWithoutArtificialLabelCount)
{
    std::array<uint8_t, 512> packet{};
    std::string maximum = std::string(63, 'a') + '.' + std::string(63, 'b') + '.' +
                          std::string(63, 'c') + '.' + std::string(61, 'd');
    EXPECT_EQ(DnsQuery(maximum, packet, 1), 12 + 255 + 4);
    EXPECT_EQ(DnsQuery(maximum + '.', packet, 1), 12 + 255 + 4);
    EXPECT_EQ(DnsQuery(maximum + 'd', packet, 1), -EINVAL);
    EXPECT_EQ(DnsQuery(std::string(64, 'a'), packet, 1), -EINVAL);
    std::string manyLabels = "a";
    for (int i = 1; i < 127; ++i) manyLabels += ".a";
    EXPECT_GT(DnsQuery(manyLabels, packet, 1), 0);
    for (std::string_view malformed : {"", ".a", "a..b", "a..", ".."})
        EXPECT_EQ(DnsQuery(malformed, packet, 1), -EINVAL) << malformed;
    EXPECT_EQ(DnsQuery(".", packet, 1), 17);
}

TEST(DnsParserTest, RejectsForwardPointersAndOversizedExpandedNames)
{
    std::array<uint8_t, 3> forward = {0xc0, 2, 0};
    size_t end;
    EXPECT_EQ((DnsName{forward, 0}.End(end)), -EPROTO);
    std::vector<uint8_t> longName;
    for (int i = 0; i < 4; ++i)
    {
        longName.push_back(63);
        longName.insert(longName.end(), 63, 'a');
    }
    longName.push_back(0);
    EXPECT_EQ((DnsName{longName, 0}.End(end)), -EPROTO);
    std::array<uint8_t, 2> reserved = {0x80, 0};
    EXPECT_EQ((DnsName{reserved, 0}.End(end)), -EPROTO);
}

TEST(DnsParserTest, ParsesBorrowedConfigWithExplicitStorage)
{
    sockaddr_in servers[2];
    ResolverConfig config;
    config.hosts = "127.0.0.1 localhost";
    EXPECT_EQ(ParseResolverConfig("nameserver 192.0.2.1 #primary\r\n"
                                  "nameserver 192.0.2.2\n"
                                  "options timeout:7 attempts:4 rotate\r\n",
                                  servers, config), 0);
    ASSERT_EQ(config.nameservers.size(), 2);
    EXPECT_EQ(config.nameservers.data(), servers);
    EXPECT_EQ(servers[0].sin_addr.s_addr, htonl(0xc0000201));
    EXPECT_EQ(servers[1].sin_addr.s_addr, htonl(0xc0000202));
    EXPECT_EQ(servers[0].sin_family, AF_INET);
    EXPECT_EQ(servers[0].sin_port, htons(53));
    EXPECT_EQ(config.timeout, std::chrono::seconds(7));
    EXPECT_EQ(config.attempts, 4);
    EXPECT_EQ(config.hosts, "127.0.0.1 localhost");
}

TEST(DnsParserTest, ConfigErrorsDoNotPublishPartialOptions)
{
    sockaddr_in storage[1];
    ResolverConfig config;
    config.attempts = 9;
    EXPECT_EQ(ParseResolverConfig("options attempts:3\nnameserver 1.2.3.4\n"
                                  "nameserver 5.6.7.8", storage, config), -ENOSPC);
    EXPECT_TRUE(config.nameservers.empty());
    EXPECT_EQ(config.attempts, 9);
    for (std::string_view invalid : {"options attempts:0", "options attempts:4294967296",
                                    "options timeout:0", "options timeout:5junk",
                                    "nameserver", "nameserver 999.1.1.1"})
        EXPECT_EQ(ParseResolverConfig(invalid, storage, config), -EINVAL) << invalid;
    EXPECT_EQ(config.attempts, 9);
}

TEST(DnsParserTest, ConfigAndHostsHaveNoFixedTextOrEntryLimit)
{
    std::string text;
    for (int i = 0; i < 1000; ++i) text += "nameserver 192.0.2.1\r\n";
    std::vector<sockaddr_in> servers(1000);
    ResolverConfig config;
    ASSERT_EQ(ParseResolverConfig(text, servers, config), 0);
    EXPECT_EQ(config.nameservers.size(), 1000);
    std::string hosts(20000, ' ');
    hosts += "\r\n192.0.2.8 long-file-final-host\r\n";
    config.hosts = hosts;
    in_addr result{};
    EXPECT_EQ(Resolve4(config, "long-file-final-host", &result), 0);
    EXPECT_EQ(result.s_addr, htonl(0xc0000208));
}

TEST(DnsParserTest, ExplicitConfigNumericAndHostsNeedNoCooperator)
{
    ResolverConfig config;
    config.attempts = 0;
    config.timeout = std::chrono::seconds(0);
    config.hosts = "192.0.2.7 some-host\r\n";
    in_addr result{htonl(42)};
    EXPECT_EQ(Resolve4(config, "192.0.2.6", &result), 0);
    EXPECT_EQ(result.s_addr, htonl(0xc0000206));
    EXPECT_EQ(Resolve4(config, "SOME-HOST.", &result), 0);
    EXPECT_EQ(result.s_addr, htonl(0xc0000207));
    EXPECT_EQ(Resolve4(config, "absent", &result), -EINVAL);
    EXPECT_EQ(result.s_addr, htonl(0xc0000207));
    config.attempts = 1;
    config.timeout = std::chrono::seconds(1);
    EXPECT_EQ(Resolve4(config, "absent", &result), -ENETUNREACH);
    EXPECT_EQ(Resolve4(config, nullptr, &result), -EINVAL);
    EXPECT_EQ(Resolve4(config, "x", nullptr), -EINVAL);
}

TEST(DnsParserTest, RetryOrderAndBudgetsAreCallerControlled)
{
    sockaddr_in servers[2]{};
    servers[0].sin_port = htons(5301);
    servers[1].sin_port = htons(5302);
    ResolverConfig config;
    config.nameservers = servers;
    config.attempts = 2;
    config.timeout = std::chrono::milliseconds(12);
    auto query = Query();
    in_addr result{};
    std::vector<int> ports;
    EXPECT_EQ(DnsAttempts(config, query, result,
        [&](const sockaddr_in& ns, std::span<const uint8_t> q, coop::time::Interval budget,
            in_addr&) -> int
        {
            EXPECT_EQ(q.data(), query.data());
            EXPECT_EQ(budget, config.timeout);
            ports.push_back(ntohs(ns.sin_port));
            return ports.size() == 4 ? -ECONNREFUSED : -ETIMEDOUT;
        }), -ECONNREFUSED);
    EXPECT_EQ(ports, (std::vector<int>{5301, 5301, 5302, 5302}));
}

TEST(DnsParserTest, TerminalResponsesDoNotTriggerHiddenFallbacks)
{
    sockaddr_in servers[2]{};
    ResolverConfig config;
    config.nameservers = servers;
    config.attempts = 4;
    auto query = Query();
    in_addr result{};
    for (int error : {0, -ENOENT, -ENODATA, -EMSGSIZE})
    {
        int exchanges = 0;
        EXPECT_EQ(DnsAttempts(config, query, result,
            [&](const sockaddr_in&, std::span<const uint8_t>, coop::time::Interval, in_addr&)
            {
                ++exchanges;
                return error;
            }), error);
        EXPECT_EQ(exchanges, 1);
    }
}

TEST(DnsParserTest, DeterministicMalformedPacketSweepIsBounded)
{
    auto query = Query();
    auto good = Reply(query);
    Record(good, {0xc0, 12}, 1, {192, 0, 2, 1});
    // Sweep every single-byte corruption through the same production parser. This is small
    // enough for normal CI and catches bounds/progress regressions under ASan/UBSan.
    for (size_t i = 0; i < good.size(); ++i)
    {
        for (unsigned byte = 0; byte < 256; ++byte)
        {
            auto packet = good;
            packet[i] = byte;
            in_addr result{htonl(42)};
            int r = DnsResponse(packet, query, result);
            if (r != 0) EXPECT_EQ(result.s_addr, htonl(42));
        }
    }
}

TEST(DnsParserTest, EmbeddedNulDoesNotTruncateConfigurationTokens)
{
    sockaddr_in storage[1];
    ResolverConfig config;
    std::string text = "nameserver 1.2.3.4";
    text += '\0';
    text += "junk";
    EXPECT_EQ(ParseResolverConfig(text, storage, config), -EINVAL);
    in_addr address{};
    EXPECT_FALSE(DnsIpv4(std::string_view("1.2.3.4\0x", 9), address));
}

} // namespace
