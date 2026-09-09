#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "coop/io/resolve.h"

namespace coop::io::detail
{

inline uint8_t DnsLower(uint8_t c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

inline bool DnsTextEqual(std::string_view a, std::string_view b)
{
    if (a.ends_with('.')) a.remove_suffix(1);
    if (b.ends_with('.')) b.remove_suffix(1);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (DnsLower(a[i]) != DnsLower(b[i])) return false;
    return true;
}

// Config tokenization operates on borrowed text, including CRLF and unterminated final lines.
// Each successful token consumes input; comments terminate their line.
//
inline std::string_view DnsToken(std::string_view& line)
{
    size_t first = line.find_first_not_of(" \t\r");
    if (first == line.npos || line[first] == '#' || line[first] == ';')
    {
        line = {};
        return {};
    }
    line.remove_prefix(first);
    size_t n = line.find_first_of(" \t\r#;");
    if (n == line.npos) n = line.size();
    auto token = line.substr(0, n);
    line.remove_prefix(n);
    return token;
}

inline std::string_view DnsLine(std::string_view& text)
{
    size_t n = text.find('\n');
    if (n == text.npos) n = text.size();
    auto line = text.substr(0, n);
    text.remove_prefix(n == text.size() ? n : n + 1);
    return line;
}

inline bool DnsIpv4(std::string_view text, in_addr& address)
{
    if (text.empty() || text.size() >= INET_ADDRSTRLEN || text.find('\0') != text.npos)
        return false;
    char number[INET_ADDRSTRLEN];
    memcpy(number, text.data(), text.size());
    number[text.size()] = 0;
    return inet_pton(AF_INET, number, &address) == 1;
}

inline int DnsHosts(std::string_view text, std::string_view name, in_addr& result)
{
    while (!text.empty())
    {
        auto line = DnsLine(text);
        in_addr address;
        if (!DnsIpv4(DnsToken(line), address)) continue;
        for (auto token = DnsToken(line); !token.empty(); token = DnsToken(line))
        {
            if (DnsTextEqual(token, name))
            {
                result = address;
                return 0;
            }
        }
    }
    return -ENOENT;
}

inline bool DnsPositive(std::string_view text, unsigned& value)
{
    unsigned n = 0;
    if (text.empty()) return false;
    for (unsigned char c : text)
    {
        if (c < '0' || c > '9' || n > (std::numeric_limits<unsigned>::max() - (c - '0')) / 10)
            return false;
        n = n * 10 + c - '0';
    }
    if (n == 0) return false;
    value = n;
    return true;
}

template<class AddServer>
int DnsConfig(std::string_view text, ResolverConfig& config, AddServer add)
{
    if (text.find('\0') != text.npos) return -EINVAL;
    ResolverConfig parsed = config;
    while (!text.empty())
    {
        auto line = DnsLine(text);
        auto key = DnsToken(line);
        if (key == "nameserver")
        {
            auto token = DnsToken(line);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(53);
            if (token.find(':') != token.npos) continue; // IPv6 is outside Resolve4's transport.
            if (!DnsIpv4(token, address.sin_addr)) return -EINVAL;
            int r = add(address);
            if (r < 0) return r;
        }
        else if (key == "options")
        {
            for (auto token = DnsToken(line); !token.empty(); token = DnsToken(line))
            {
                unsigned value;
                if (token.starts_with("timeout:"))
                {
                    if (!DnsPositive(token.substr(8), value)) return -EINVAL;
                    parsed.timeout = std::chrono::seconds(value);
                }
                else if (token.starts_with("attempts:"))
                {
                    if (!DnsPositive(token.substr(9), value)) return -EINVAL;
                    parsed.attempts = value;
                }
            }
        }
    }
    config = parsed;
    return 0;
}

inline uint16_t DnsU16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }

// A DNS name remains a view of the packet. Compression is traversed without expanding strings.
// Pointers must refer backwards (RFC 1035 4.1.4); every pointer also lowers the traversal ceiling.
// That invariant rejects pointer cycles even when a cycle contains labels, and limits pointer-only
// chains by packet size. Expanded length is bounded by DNS's 255-octet wire-name limit.
//
struct DnsName
{
    std::span<const uint8_t> packet;
    size_t offset;

    struct Reader
    {
        std::span<const uint8_t> packet;
        size_t at;
        size_t ceiling;
        size_t wireEnd = 0;
        unsigned remaining = 0;
        unsigned expanded = 0;
        bool done = false;

        int Next(uint8_t& value)
        {
            if (done) return 0;
            for (;;)
            {
                if (at >= packet.size()) return -EPROTO;
                uint8_t c = packet[at++];
                if (remaining)
                {
                    --remaining;
                    value = DnsLower(c);
                }
                else if ((c & 0xc0) == 0xc0)
                {
                    if (at >= packet.size()) return -EPROTO;
                    size_t target = size_t(c & 0x3f) << 8 | packet[at++];
                    if (target >= at - 2 || target >= ceiling) return -EPROTO;
                    if (!wireEnd) wireEnd = at;
                    at = ceiling = target;
                    continue;
                }
                else
                {
                    if (c > 63) return -EPROTO;
                    value = c;
                    remaining = c;
                    if (c == 0)
                    {
                        done = true;
                        if (!wireEnd) wireEnd = at;
                    }
                }
                if (++expanded > 255) return -EPROTO;
                return 1;
            }
        }
    };

    Reader Read() const { return {packet, offset, packet.size()}; }

    int End(size_t& end) const
    {
        auto reader = Read();
        uint8_t c;
        int r;
        while ((r = reader.Next(c)) > 0) {}
        if (r < 0) return r;
        end = reader.wireEnd;
        return 0;
    }

    bool Equal(DnsName other) const
    {
        auto a = Read();
        auto b = other.Read();
        uint8_t ac, bc;
        for (;;)
        {
            int ar = a.Next(ac), br = b.Next(bc);
            if (ar != br || ar < 0) return false;
            if (ar == 0) return true;
            if (ac != bc) return false;
        }
    }
};

inline int DnsQuery(std::string_view hostname, std::span<uint8_t> packet, uint16_t id)
{
    if (hostname.empty() || packet.size() < 17) return -EINVAL;
    if (hostname.ends_with('.')) hostname.remove_suffix(1);
    size_t at = 12;
    while (!hostname.empty())
    {
        size_t n = hostname.find('.');
        if (n == hostname.npos) n = hostname.size();
        if (n == 0 || n > 63 || at + 1 + n + 5 > packet.size()) return -EINVAL;
        packet[at++] = static_cast<uint8_t>(n);
        for (unsigned char c : hostname.substr(0, n))
        {
            if (c == 0) return -EINVAL;
            packet[at++] = c;
        }
        if (n != hostname.size() && n + 1 == hostname.size()) return -EINVAL;
        hostname.remove_prefix(n == hostname.size() ? n : n + 1);
    }
    if (at - 12 + 1 > 255) return -EINVAL;
    packet[at++] = 0;
    packet[at++] = 0; packet[at++] = 1; // A
    packet[at++] = 0; packet[at++] = 1; // IN
    memset(packet.data(), 0, 12);
    packet[0] = id >> 8; packet[1] = id & 0xff;
    packet[2] = 1; // recursion desired
    packet[5] = 1;
    return static_cast<int>(at);
}

struct DnsRecord
{
    DnsName name;
    uint16_t type = 0;
    uint16_t klass = 0;
    size_t data = 0;
    size_t end = 0;

    int Parse()
    {
        auto packet = name.packet;
        size_t at;
        if (name.End(at) < 0 || packet.size() - at < 10) return -EPROTO;
        type = DnsU16(&packet[at]);
        klass = DnsU16(&packet[at + 2]);
        size_t length = DnsU16(&packet[at + 8]);
        data = at + 10;
        if (length > packet.size() - data) return -EPROTO;
        end = data + length;
        if (klass == 1 && type == 1 && length != 4) return -EPROTO;
        if (klass == 1 && type == 5)
        {
            size_t nameEnd;
            if (DnsName{packet, data}.End(nameEnd) < 0 || nameEnd != end) return -EPROTO;
        }
        return 0;
    }
};

// Match the transaction and question before accepting any answer, including negative answers.
// Re-scan the small UDP answer section for each CNAME hop: no heap, graph, or expanded name copies.
// The answer count bounds CNAME cycles; unrelated/additional A records are never address results.
//
inline int DnsResponse(std::span<const uint8_t> packet, std::span<const uint8_t> query,
                       in_addr& result)
{
    if (packet.size() < 12 || query.size() < 17 || DnsU16(packet.data()) != DnsU16(query.data()) ||
        (packet[2] & 0xf8) != 0x80 || (packet[3] & 0x40) || DnsU16(&packet[4]) != 1)
        return -EPROTO;
    size_t at;
    DnsName question{packet, 12};
    if (question.End(at) < 0 || packet.size() - at < 4 ||
        !question.Equal({query, 12}) || DnsU16(&packet[at]) != 1 || DnsU16(&packet[at + 2]) != 1)
        return -EPROTO;
    at += 4;
    if (packet[2] & 2) return -EMSGSIZE;
    size_t answersAt = at;
    unsigned answers = DnsU16(&packet[6]);
    unsigned records = answers + unsigned(DnsU16(&packet[8])) + DnsU16(&packet[10]);
    for (unsigned i = 0; i < records; ++i)
    {
        DnsRecord rr{{packet, at}};
        if (rr.Parse() < 0) return -EPROTO;
        at = rr.end;
    }
    if (at != packet.size()) return -EPROTO;
    unsigned rcode = packet[3] & 15;
    if (rcode == 3) return -ENOENT;
    if (rcode == 2) return -EAGAIN; // SERVFAIL, eligible for configured retry.
    if (rcode == 5) return -EACCES;
    if (rcode != 0) return -EPROTO;

    DnsName wanted = question;
    for (unsigned hop = 0; hop <= answers; ++hop)
    {
        at = answersAt;
        size_t address = 0, alias = 0;
        for (unsigned i = 0; i < answers; ++i)
        {
            DnsRecord rr{{packet, at}};
            if (rr.Parse() < 0) return -EPROTO;
            at = rr.end;
            if (rr.klass != 1 || !rr.name.Equal(wanted)) continue;
            if (rr.type == 1 && !address) address = rr.data;
            if (rr.type == 5)
            {
                if (alias && !DnsName{packet, alias}.Equal({packet, rr.data})) return -EPROTO;
                alias = rr.data;
            }
        }
        if (address && alias) return -EPROTO;
        if (address)
        {
            memcpy(&result, &packet[address], 4);
            return 0;
        }
        if (!alias) return -ENODATA;
        wanted = {packet, alias};
    }
    return -EPROTO;
}

// Retry ordering is driven solely by the supplied configuration. Terminal DNS results do not
// trigger a different protocol or an additional query. Preserve the final failure's identity.
// The exchange is statically dispatched, so deterministic tests exercise the production loop.
//
template<class Exchange>
int DnsAttempts(const ResolverConfig& config, std::span<const uint8_t> query,
                in_addr& result, Exchange exchange)
{
    int last = -ENETUNREACH;
    for (const auto& server : config.nameservers)
    {
        for (unsigned attempt = 0; attempt < config.attempts; ++attempt)
        {
            last = exchange(server, query, config.timeout, result);
            if (last == 0 || last == -ENOENT || last == -ENODATA || last == -EMSGSIZE)
                return last;
        }
    }
    return last;
}

} // namespace coop::io::detail
