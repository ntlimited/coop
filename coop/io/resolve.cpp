#include "resolve.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/random.h>
#include <sys/socket.h>
#include <vector>

#include "coop/cooperator_var.hpp"
#include "coop/coordinator.h"
#include "coop/self.h"

#include "connect.h"
#include "descriptor.h"
#include "detail/dns.hpp"
#include "open.h"
#include "read.h"
#include "recv.h"
#include "send.h"

namespace coop::io
{

int ParseResolverConfig(std::string_view text, std::span<sockaddr_in> storage,
                        ResolverConfig& config)
{
    size_t used = 0;
    ResolverConfig parsed = config;
    int r = detail::DnsConfig(text, parsed, [&](sockaddr_in address)
    {
        if (used == storage.size()) return -ENOSPC;
        storage[used++] = address;
        return 0;
    });
    if (r < 0) return r;
    parsed.nameservers = storage.first(used);
    config = parsed;
    return 0;
}

namespace
{

// Only the legacy convenience path owns configuration. The eager per-cooperator cost is one
// pointer; its state and contiguous backing storage are allocated only on the first hostname
// lookup. Publication happens after all cooperative setup I/O succeeds, under the coordinator.
// DNS queries never mutate configuration, so lookups release the setup gate before doing IO.
//
struct DefaultResolver
{
    Coordinator loading;
    bool ready = false;
    std::string hosts;
    std::vector<sockaddr_in> nameservers;
    ResolverConfig config;
};

CooperatorVar<std::unique_ptr<DefaultResolver>> s_defaultResolver;

struct SetupGuard
{
    Coordinator& coord;
    Context* context = Self();
    explicit SetupGuard(Coordinator& c) : coord(c) { coord.Acquire(context); }
    ~SetupGuard() { coord.Release(context, false); }
};

int LoadText(const char* path, std::string& text)
{
    int fd = Open(path, O_RDONLY);
    if (fd < 0) return fd;
    Descriptor file(fd);
    char chunk[1024];
    uint64_t offset = 0;
    for (;;)
    {
        int n = Read(file, chunk, sizeof(chunk), offset);
        if (n < 0) return n;
        if (n == 0) return 0;
        text.append(chunk, static_cast<size_t>(n));
        offset += static_cast<unsigned>(n);
    }
}

int DefaultConfig(ResolverConfig& config)
{
    auto& owner = *s_defaultResolver;
    if (!owner) owner = std::make_unique<DefaultResolver>();
    auto& state = *owner;
    if (!state.ready)
    {
        SetupGuard guard(state.loading);
        if (!state.ready)
        {
            // Build privately: a failed read/parse leaves no half-loaded configuration behind.
            // Missing hosts is allowed; unreadable or malformed resolver configuration is not
            // silently replaced with an empty successful snapshot.
            //
            std::string hosts, resolv;
            std::vector<sockaddr_in> nameservers;
            ResolverConfig parsed;
            int r = LoadText("/etc/hosts", hosts);
            if (r < 0 && r != -ENOENT) return r;
            r = LoadText("/etc/resolv.conf", resolv);
            if (r < 0) return r;
            r = detail::DnsConfig(resolv, parsed, [&](sockaddr_in address)
            {
                nameservers.push_back(address);
                return 0;
            });
            if (r < 0) return r;
            if (nameservers.empty())
            {
                // Preserve the convenience resolver's local-stub fallback. Explicit configs
                // have no fallback: an empty nameserver span is -ENETUNREACH when DNS is needed.
                //
                sockaddr_in fallback{};
                fallback.sin_family = AF_INET;
                fallback.sin_port = htons(53);
                fallback.sin_addr.s_addr = htonl(0x7f000035); // 127.0.0.53
                nameservers.push_back(fallback);
            }
            state.hosts = std::move(hosts);
            state.nameservers = std::move(nameservers);
            parsed.hosts = state.hosts;
            parsed.nameservers = state.nameservers;
            state.config = parsed;
            state.ready = true;
        }
    }
    config = state.config;
    return 0;
}

int Numeric(const char* hostname, in_addr* result)
{
    if (!hostname || !result || !*hostname) return -EINVAL;
    in_addr address;
    if (inet_pton(AF_INET, hostname, &address) != 1) return 1;
    *result = address;
    return 0;
}

int TransactionId(uint16_t& id)
{
    // DNS IDs must not use the predictable process-global rand() sequence. GRND_NONBLOCK makes
    // random-source readiness an explicit error rather than a hidden block on the cooperator.
    // The UDP socket's ephemeral source port supplies the other half of reply association.
    //
    ssize_t n;
    do { n = getrandom(&id, sizeof(id), GRND_NONBLOCK); } while (n < 0 && errno == EINTR);
    if (n < 0) return -errno;
    return n == sizeof(id) ? 0 : -EIO;
}

int Exchange(const sockaddr_in& server, std::span<const uint8_t> query,
             time::Interval timeout, in_addr& result)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    Descriptor desc(fd);
    // Keep one attempt budget across all of its ring operations. Comparing elapsed time avoids
    // overflow in now + a caller-supplied large interval.
    //
    auto start = std::chrono::steady_clock::now();
    auto remaining = [&]
    {
        auto elapsed = std::chrono::duration_cast<time::Interval>(
            std::chrono::steady_clock::now() - start);
        return elapsed < timeout ? timeout - elapsed : time::Interval::zero();
    };
    auto budget = remaining();
    if (budget <= time::Interval::zero()) return -ETIMEDOUT;
    int r = Connect(desc, reinterpret_cast<const sockaddr*>(&server), sizeof(server), budget);
    if (r < 0) return r;
    budget = remaining();
    if (budget <= time::Interval::zero()) return -ETIMEDOUT;
    r = Send(desc, query.data(), query.size(), 0, budget);
    if (r < 0) return r;
    if (static_cast<size_t>(r) != query.size()) return -EIO;
    budget = remaining();
    if (budget <= time::Interval::zero()) return -ETIMEDOUT;
    // Classical DNS UDP has a protocol-defined 512-byte limit without EDNS. MSG_TRUNC exposes
    // a larger datagram rather than treating a truncated local copy as a complete packet.
    //
    uint8_t response[512];
    r = Recv(desc, response, sizeof(response), MSG_TRUNC, budget);
    if (r < 0) return r;
    if (static_cast<size_t>(r) > sizeof(response)) return -EMSGSIZE;
    return detail::DnsResponse({response, static_cast<size_t>(r)}, query, result);
}

} // namespace

int Resolve4(const ResolverConfig& config, const char* hostname, in_addr* result)
{
    int r = Numeric(hostname, result);
    if (r <= 0) return r;
    in_addr address;
    if (detail::DnsHosts(config.hosts, hostname, address) == 0)
    {
        *result = address;
        return 0;
    }
    if (config.attempts == 0 || config.timeout <= time::Interval::zero()) return -EINVAL;
    if (config.nameservers.empty()) return -ENETUNREACH;
    for (const auto& ns : config.nameservers)
        if (ns.sin_family != AF_INET || ns.sin_port == 0) return -EINVAL;

    uint8_t query[12 + 255 + 4];
    // Validate the supplied name before drawing randomness or doing network IO.
    //
    int n = detail::DnsQuery(hostname, query, 0);
    if (n < 0) return n;
    uint16_t id;
    r = TransactionId(id);
    if (r < 0) return r;
    query[0] = id >> 8;
    query[1] = id & 255;
    r = detail::DnsAttempts(config, {query, static_cast<size_t>(n)}, address, Exchange);
    if (r == 0) *result = address;
    return r;
}

int Resolve4(const char* hostname, in_addr* result)
{
    int r = Numeric(hostname, result);
    if (r <= 0) return r;
    ResolverConfig config;
    r = DefaultConfig(config);
    return r < 0 ? r : Resolve4(config, hostname, result);
}

int Resolve4(const char* hostname, in_addr* result, time::Interval timeout)
{
    int r = Numeric(hostname, result);
    if (r <= 0) return r;
    ResolverConfig config;
    r = DefaultConfig(config);
    if (r < 0) return r;
    config.timeout = timeout;
    return Resolve4(config, hostname, result);
}

} // namespace coop::io
