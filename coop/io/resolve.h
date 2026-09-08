#pragma once

#include <netinet/in.h>
#include <span>
#include <string_view>

#include "coop/time/interval.h"

namespace coop::io
{

// An immutable borrowed configuration, safe to share between concurrent lookups. The caller
// keeps both spans alive and unchanged until every lookup finishes. No address cache or config
// file I/O is attached to this view. Each nameserver includes its UDP port (normally htons(53)).
//
struct ResolverConfig
{
    std::span<const sockaddr_in> nameservers;
    std::string_view hosts;
    time::Interval timeout = std::chrono::seconds(5);
    unsigned attempts = 2;
};

// Parse IPv4 nameserver lines and options timeout:/attempts: from resolv.conf text. Unknown
// directives/options and IPv6 nameservers are ignored. Returns 0 or negative errno; insufficient
// caller storage is -ENOSPC. On success config borrows storage and keeps its hosts view. On failure
// the config view is unchanged; storage (including storage it already views) may have been written. Parsing allocates nothing and performs no I/O.
//
int ParseResolverConfig(std::string_view text, std::span<sockaddr_in> storage,
                        ResolverConfig& config);

// Resolve one IPv4 address using only the supplied configuration. Numeric addresses bypass all
// setup and I/O. Hosts lookup is case-insensitive and accepts a trailing dot. DNS is a single
// recursive A query over UDP; associated CNAMEs present in that reply are followed without copies.
// timeout is a fresh budget per nameserver attempt, shared by connect/send/recv, not a total lookup
// deadline. attempts==0 or timeout<=0 is -EINVAL when DNS is needed. No implicit TCP/EDNS, search
// suffixes, DNSSEC validation, retries beyond attempts, or subsequent CNAME queries.
//
// Returns 0 on success and leaves result unchanged on failure: -ENOENT NXDOMAIN, -ENODATA no
// associated A answer, -EMSGSIZE truncated reply (caller chooses fallback), -EPROTO malformed
// response, or transport errno. Empty nameservers is -ENETUNREACH. Plain waits are not kill-aware.
//
int Resolve4(const ResolverConfig& config, const char* hostname, in_addr* result);

// Convenience configuration: /etc/hosts and /etc/resolv.conf are loaded lazily once per
// cooperator. Setup owns its text/address storage; concurrent first calls wait for initialization.
// Failed setup is not published and can be retried. There is no DNS answer cache or live reload.
// Explicit configuration above avoids these allocations and file reads entirely.
//
int Resolve4(const char* hostname, in_addr* result);
int Resolve4(const char* hostname, in_addr* result, time::Interval timeout);

} // namespace coop::io
