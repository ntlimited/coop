#include "resolve.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "coop/self.h"

#include "close.h"
#include "connect.h"
#include "descriptor.h"
#include "read_file.h"
#include "recv.h"
#include "send.h"

namespace coop
{

namespace io
{

// -------------------------------------------------------------------------------------
// Config state — lazily parsed on first Resolve4 call
// -------------------------------------------------------------------------------------

struct ResolvConfig
{
    std::vector<struct sockaddr_in> nameservers;
    int timeoutSec  = 5;
    int attempts    = 2;
    bool loaded     = false;
};

struct HostsConfig
{
    std::unordered_map<std::string, struct in_addr> entries;
    bool loaded = false;
};

static ResolvConfig s_resolv;
static HostsConfig  s_hosts;

// -------------------------------------------------------------------------------------
// Config parsers
// -------------------------------------------------------------------------------------

static void ParseResolvConf()
{
    if (s_resolv.loaded) return;
    s_resolv.loaded = true;

    char buf[4096];
    int len = ReadFile("/etc/resolv.conf", buf, sizeof(buf) - 1);
    if (len < 0)
    {
        spdlog::warn("resolve: failed to read /etc/resolv.conf err={}", len);
        return;
    }
    buf[len] = '\0';

    char* line = buf;
    while (line && *line)
    {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Skip leading whitespace
        //
        while (*line == ' ' || *line == '\t') line++;

        if (strncmp(line, "nameserver", 10) == 0 && (line[10] == ' ' || line[10] == '\t'))
        {
            char* ip = line + 11;
            while (*ip == ' ' || *ip == '\t') ip++;

            // Trim trailing whitespace
            //
            char* end = ip + strlen(ip) - 1;
            while (end > ip && (*end == ' ' || *end == '\t' || *end == '\r')) *end-- = '\0';

            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(53);
            if (inet_pton(AF_INET, ip, &addr.sin_addr) == 1)
            {
                s_resolv.nameservers.push_back(addr);
                spdlog::debug("resolve: nameserver {}", ip);
            }
        }
        else if (strncmp(line, "options", 7) == 0 && (line[7] == ' ' || line[7] == '\t'))
        {
            char* opts = line + 8;

            char* timeout = strstr(opts, "timeout:");
            if (timeout)
            {
                int val = atoi(timeout + 8);
                if (val > 0) s_resolv.timeoutSec = val;
            }

            char* attempts = strstr(opts, "attempts:");
            if (attempts)
            {
                int val = atoi(attempts + 9);
                if (val > 0) s_resolv.attempts = val;
            }
        }

        line = nl ? nl + 1 : nullptr;
    }

    if (s_resolv.nameservers.empty())
    {
        spdlog::warn("resolve: no nameservers found, adding 127.0.0.53 as fallback");
        struct sockaddr_in fallback = {};
        fallback.sin_family = AF_INET;
        fallback.sin_port = htons(53);
        inet_pton(AF_INET, "127.0.0.53", &fallback.sin_addr);
        s_resolv.nameservers.push_back(fallback);
    }
}

static void ParseHosts()
{
    if (s_hosts.loaded) return;
    s_hosts.loaded = true;

    char buf[8192];
    int len = ReadFile("/etc/hosts", buf, sizeof(buf) - 1);
    if (len < 0)
    {
        spdlog::warn("resolve: failed to read /etc/hosts err={}", len);
        return;
    }
    buf[len] = '\0';

    char* line = buf;
    while (line && *line)
    {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Skip leading whitespace
        //
        while (*line == ' ' || *line == '\t') line++;

        // Skip comments and blank lines
        //
        if (*line == '#' || *line == '\0')
        {
            line = nl ? nl + 1 : nullptr;
            continue;
        }

        // Parse: <ip> <hostname> [aliases...]
        //
        char* ip = line;
        char* sep = ip;
        while (*sep && *sep != ' ' && *sep != '\t') sep++;
        if (!*sep)
        {
            line = nl ? nl + 1 : nullptr;
            continue;
        }
        *sep = '\0';

        struct in_addr addr;
        if (inet_pton(AF_INET, ip, &addr) != 1)
        {
            // Could be IPv6 — skip
            //
            line = nl ? nl + 1 : nullptr;
            continue;
        }

        // Walk remaining tokens as hostnames
        //
        char* tok = sep + 1;
        while (*tok)
        {
            while (*tok == ' ' || *tok == '\t') tok++;
            if (!*tok || *tok == '#') break;

            char* end = tok;
            while (*end && *end != ' ' && *end != '\t' && *end != '#' && *end != '\r') end++;

            std::string name(tok, end - tok);
            s_hosts.entries[name] = addr;
            spdlog::debug("resolve: hosts entry {} -> {}", name, ip);

            tok = end;
        }

        line = nl ? nl + 1 : nullptr;
    }
}

// -------------------------------------------------------------------------------------
// DNS packet construction
// -------------------------------------------------------------------------------------

// Encode a hostname into DNS wire format (length-prefixed labels). Returns bytes written,
// or 0 on error (hostname too long, empty label, etc).
//
static int EncodeDnsName(const char* hostname, uint8_t* buf, int bufSize)
{
    int pos = 0;
    const char* p = hostname;

    while (*p)
    {
        const char* dot = strchr(p, '.');
        int labelLen = dot ? static_cast<int>(dot - p) : static_cast<int>(strlen(p));

        if (labelLen == 0)
        {
            // Trailing dot — just skip it
            //
            if (dot)
            {
                p = dot + 1;
                continue;
            }
            break;
        }

        if (labelLen > 63 || pos + 1 + labelLen >= bufSize)
        {
            return 0;
        }

        buf[pos++] = static_cast<uint8_t>(labelLen);
        memcpy(buf + pos, p, labelLen);
        pos += labelLen;

        p = dot ? dot + 1 : p + labelLen;
    }

    if (pos + 1 > bufSize) return 0;
    buf[pos++] = 0; // root label
    return pos;
}

// Build a DNS A-record query. Returns total packet length, or 0 on error.
//
static int BuildDnsQuery(const char* hostname, uint8_t* buf, int bufSize, uint16_t txnId)
{
    if (bufSize < 12) return 0;

    // Header (12 bytes)
    //
    buf[0] = txnId >> 8;
    buf[1] = txnId & 0xFF;
    buf[2] = 0x01; // flags: RD (recursion desired)
    buf[3] = 0x00;
    buf[4] = 0x00; buf[5] = 0x01; // QDCOUNT = 1
    buf[6] = 0x00; buf[7] = 0x00; // ANCOUNT = 0
    buf[8] = 0x00; buf[9] = 0x00; // NSCOUNT = 0
    buf[10] = 0x00; buf[11] = 0x00; // ARCOUNT = 0

    int nameLen = EncodeDnsName(hostname, buf + 12, bufSize - 12 - 4);
    if (nameLen == 0) return 0;

    int pos = 12 + nameLen;
    if (pos + 4 > bufSize) return 0;

    // QTYPE = A (1)
    //
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;

    // QCLASS = IN (1)
    //
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;

    return pos;
}

// -------------------------------------------------------------------------------------
// DNS response parsing
// -------------------------------------------------------------------------------------

// Skip a DNS name in wire format, handling compression pointers. Returns the number of bytes
// consumed from the current position, or 0 on error.
//
static int SkipDnsName(const uint8_t* pkt, int pktLen, int offset)
{
    int pos = offset;
    int jumped = 0;
    int bytesConsumed = 0;

    while (pos < pktLen)
    {
        uint8_t len = pkt[pos];

        if (len == 0)
        {
            // End of name
            //
            if (!jumped) bytesConsumed = pos - offset + 1;
            return bytesConsumed ? bytesConsumed : 1;
        }

        if ((len & 0xC0) == 0xC0)
        {
            // Compression pointer
            //
            if (pos + 1 >= pktLen) return 0;
            if (!jumped) bytesConsumed = pos - offset + 2;
            int ptr = ((len & 0x3F) << 8) | pkt[pos + 1];
            if (ptr >= pktLen) return 0;
            pos = ptr;
            jumped = 1;
            continue;
        }

        if (len > 63) return 0;
        pos += 1 + len;
    }

    return 0;
}

// Parse a DNS response and extract the first A record. Returns 0 on success, negative errno
// on failure.
//
static int ParseDnsResponse(const uint8_t* pkt, int pktLen, uint16_t expectedId,
                            struct in_addr* result)
{
    if (pktLen < 12)
    {
        spdlog::debug("resolve: response too short len={}", pktLen);
        return -EPROTO;
    }

    uint16_t id = (pkt[0] << 8) | pkt[1];
    if (id != expectedId)
    {
        spdlog::debug("resolve: id mismatch expected={} got={}", expectedId, id);
        return -EPROTO;
    }

    uint8_t flags1 = pkt[2];
    uint8_t flags2 = pkt[3];

    // Check QR bit (must be 1 = response)
    //
    if (!(flags1 & 0x80))
    {
        spdlog::debug("resolve: QR bit not set");
        return -EPROTO;
    }

    // Check RCODE
    //
    int rcode = flags2 & 0x0F;
    if (rcode == 3) // NXDOMAIN
    {
        spdlog::debug("resolve: NXDOMAIN");
        return -ENOENT;
    }
    if (rcode != 0)
    {
        spdlog::debug("resolve: DNS error rcode={}", rcode);
        return -EPROTO;
    }

    uint16_t qdcount = (pkt[4] << 8) | pkt[5];
    uint16_t ancount = (pkt[6] << 8) | pkt[7];

    // Skip question section
    //
    int pos = 12;
    for (int i = 0; i < qdcount; i++)
    {
        int nameLen = SkipDnsName(pkt, pktLen, pos);
        if (nameLen == 0) return -EPROTO;
        pos += nameLen;
        pos += 4; // QTYPE + QCLASS
        if (pos > pktLen) return -EPROTO;
    }

    // Walk answer RRs looking for the first A record
    //
    for (int i = 0; i < ancount; i++)
    {
        int nameLen = SkipDnsName(pkt, pktLen, pos);
        if (nameLen == 0) return -EPROTO;
        pos += nameLen;

        if (pos + 10 > pktLen) return -EPROTO;

        uint16_t rrtype  = (pkt[pos] << 8) | pkt[pos + 1];
        uint16_t rrclass = (pkt[pos + 2] << 8) | pkt[pos + 3];
        // TTL at pos+4..pos+7 (unused)
        uint16_t rdlen   = (pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;

        if (pos + rdlen > pktLen) return -EPROTO;

        if (rrtype == 1 && rrclass == 1 && rdlen == 4)
        {
            // A record — copy the 4-byte IPv4 address
            //
            memcpy(result, pkt + pos, 4);
            return 0;
        }

        pos += rdlen;
    }

    spdlog::debug("resolve: no A record in {} answers", ancount);
    return -ENOENT;
}

// -------------------------------------------------------------------------------------
// Transport
// -------------------------------------------------------------------------------------

static int DoResolve4(const char* hostname, struct in_addr* result, time::Interval timeout)
{
    // Fast path: numeric address
    //
    if (inet_pton(AF_INET, hostname, result) == 1)
    {
        return 0;
    }

    // Check /etc/hosts
    //
    ParseHosts();
    auto it = s_hosts.entries.find(hostname);
    if (it != s_hosts.entries.end())
    {
        *result = it->second;
        spdlog::debug("resolve: {} found in /etc/hosts", hostname);
        return 0;
    }

    // DNS query
    //
    ParseResolvConf();

    // Build query packet
    //
    uint16_t txnId = static_cast<uint16_t>(rand() & 0xFFFF);
    uint8_t query[512];
    int queryLen = BuildDnsQuery(hostname, query, sizeof(query), txnId);
    if (queryLen == 0)
    {
        spdlog::warn("resolve: failed to build query for {}", hostname);
        return -EINVAL;
    }

    // Try each nameserver
    //
    for (auto& ns : s_resolv.nameservers)
    {
        for (int attempt = 0; attempt < s_resolv.attempts; attempt++)
        {
            int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
            if (fd < 0)
            {
                spdlog::warn("resolve: socket() failed errno={}", errno);
                return -errno;
            }

            auto* ring = GetUring();
            Descriptor desc(fd, ring);

            // UDP connect just sets the destination address
            //
            int ret = Connect(desc, (struct sockaddr*)&ns, sizeof(ns));
            if (ret < 0)
            {
                spdlog::debug("resolve: connect to nameserver failed ret={}", ret);
                desc.Close();
                continue;
            }

            ret = Send(desc, query, queryLen);
            if (ret < 0)
            {
                spdlog::debug("resolve: send failed ret={}", ret);
                desc.Close();
                continue;
            }

            uint8_t response[512];
            ret = Recv(desc, response, sizeof(response), 0, timeout);
            desc.Close();

            if (ret == -ETIMEDOUT)
            {
                spdlog::debug("resolve: timeout from nameserver, attempt {}", attempt + 1);
                continue;
            }

            if (ret < 0)
            {
                spdlog::debug("resolve: recv failed ret={}", ret);
                continue;
            }

            int parseRet = ParseDnsResponse(response, ret, txnId, result);
            if (parseRet == 0)
            {
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, result, ipStr, sizeof(ipStr));
                spdlog::debug("resolve: {} -> {}", hostname, ipStr);
                return 0;
            }

            // NXDOMAIN is authoritative — don't retry other nameservers
            //
            if (parseRet == -ENOENT)
            {
                return parseRet;
            }

            spdlog::debug("resolve: parse error ret={}, retrying", parseRet);
        }
    }

    spdlog::warn("resolve: all nameservers exhausted for {}", hostname);
    return -ETIMEDOUT;
}

int Resolve4(const char* hostname, struct in_addr* result)
{
    auto timeout = std::chrono::seconds(s_resolv.loaded ? s_resolv.timeoutSec : 5);
    return DoResolve4(hostname, result, std::chrono::duration_cast<time::Interval>(timeout));
}

int Resolve4(const char* hostname, struct in_addr* result, time::Interval timeout)
{
    return DoResolve4(hostname, result, timeout);
}

} // end namespace coop::io
} // end namespace coop
