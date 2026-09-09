# IPv4 DNS resolution

`io::Resolve4` provides a small cooperative IPv4 resolver. Explicit configuration lets a caller
choose its nameservers, setup storage and retry budget; the convenience overloads supply system
configuration for simple clients. Both use the same bounded packet parser. Neither caches DNS
answers, creates background contexts, follows search suffixes or silently switches protocols.

## Caller-owned configuration

```cpp
#include <arpa/inet.h>
#include <chrono>
#include "coop/io/resolve.h"

sockaddr_in server{};
server.sin_family = AF_INET;
server.sin_port = htons(53);
inet_pton(AF_INET, "192.0.2.53", &server.sin_addr);

coop::io::ResolverConfig config;
config.nameservers = {&server, 1};
config.hosts = "192.0.2.10 backend.internal\n"; // optional borrowed hosts-file text
config.timeout = std::chrono::milliseconds(250);
config.attempts = 1; // one attempt per nameserver, in caller order

in_addr address;
int error = coop::io::Resolve4(config, "backend.internal", &address);
if (error < 0) {
    // The caller selects its fallback or reports the specific failure.
}
```

The view does not own its nameserver span or hosts text. Keep their storage alive and unchanged
until every lookup using it has completed. Multiple contexts or cooperators can share an immutable
configuration without locks; each query keeps its own packet and transport state. An explicit
configuration performs no setup file reads and allocates no configuration or packet storage.
Numeric addresses and hosts hits do not require a running cooperator. Network lookups do.

`ParseResolverConfig(text, nameserverStorage, config)` accepts borrowed resolv.conf text and
caller-owned contiguous `sockaddr_in` storage. It parses IPv4 `nameserver` lines and the `timeout:`
(seconds) and `attempts:` options. CRLF, comments and a final line without a newline are accepted.
Unknown options/directives and IPv6 nameservers are ignored. Malformed supported values return
`-EINVAL`; insufficient nameserver storage returns `-ENOSPC`. There is no fixed nameserver count
or text size limit. On success the config views the populated prefix of the supplied storage;
its hosts view is preserved. On failure the config view object is unchanged, but supplied storage
may have been written, including storage that an existing config views. Parse before sharing it.

Host lookup scans borrowed text without building a hash table or copying names. Matching is ASCII
case-insensitive and accepts a final dot; the first matching IPv4 entry wins. This costs a linear
scan per hosts lookup. Leave the hosts view empty to pay none of that scan, or perform application
host mapping before calling the resolver.

## Convenience setup

`Resolve4(hostname, &address)` and `Resolve4(hostname, &address, timeout)` lazily read `/etc/hosts`
and `/etc/resolv.conf` once per cooperator. The eager per-cooperator state is one pointer; owned
setup state, file text and nameserver storage are allocated on first hostname use. Files are read
cooperatively in chunks without a fixed file size cap. Setup is serialized by a coordinator;
other first callers wait until the complete configuration has been published. A failed read or
parse is returned and leaves initialization retryable. A missing hosts file is treated as empty.
The convenience defaults are two attempts, five seconds per attempt, and `127.0.0.53:53` if a
successfully parsed resolver file contains no IPv4 nameservers. Explicit configurations have no
such nameserver fallback. The timeout overload overrides the loaded timeout for that lookup only.

The retained snapshot is configuration, not a DNS answer cache. It is destroyed with the owning
cooperator and is not automatically reloaded. Applications that need reloadable configuration
construct and publish their own immutable views with appropriate backing-storage lifetimes.

## Completion, retry and protocol contracts

A zero result means the address is available. Every failure leaves the output address unchanged.
Each nameserver receives up to `attempts` tries before the next nameserver is considered. One
attempt's `timeout` budget is shared across UDP connect, send and receive; it is not restarted on
each individual operation. It does not include configuration loading, descriptor cleanup or later
attempts. There is no total lookup deadline, and these are plain cooperative waits, not kill-aware
waits. DNS requires positive attempts and timeout. Numeric and hosts hits do not use those options.

| Result | Meaning |
| --- | --- |
| `-ENOENT` | Matching DNS reply reports NXDOMAIN. |
| `-ENODATA` | Matching reply has no associated IPv4 answer, including a CNAME without its target A record. |
| `-EMSGSIZE` | TC is set, or the received UDP datagram exceeds classical DNS's 512-byte limit. |
| `-EPROTO` | Malformed reply, mismatched transaction/question, invalid compression or alias cycle. |
| `-EAGAIN` | DNS SERVFAIL, or an underlying operation returned EAGAIN. |
| `-EACCES` | DNS REFUSED, or an underlying operation returned EACCES. |
| `-ENETUNREACH` | An explicit configuration has no nameservers and DNS is needed. |
| Other negative errno | Setup, socket or ring operation failure, including timeout. |

Success, NXDOMAIN, no-data and truncation are terminal. Other errors permit the configured retries;
if those are exhausted, the final error is returned instead of being relabeled as a timeout.
A truncated reply is an explicit caller decision: no hidden TCP fallback or EDNS negotiation occurs.
A CNAME chain is followed only within the response's answer section; no subsequent query is sent.

Queries use a connected UDP socket, an OS-selected ephemeral source port and a random transaction
ID from nonblocking `getrandom`. Failure to obtain randomness is returned. Reply parsing checks
transaction ID, QR/opcode, the exact case-insensitive question name, type and class before accepting
positive or negative answers. Only A records owned by that question or its associated CNAME chain
can supply the result. Authority/additional addresses and unrelated answer records cannot.

Names and aliases remain views of the received packet. Compression traversal makes bounded
progress and enforces DNS's label/name size limits; it never expands names into owned strings.
The parser validates record bounds before publishing an address. It rescans the small classical
UDP answer section for CNAME hops instead of allocating an alias graph. Those protocol checks do
not add work to unrelated IO paths.

This is a recursive-server IPv4 UDP client, not a general system resolver or DNSSEC validator.
Applications needing IPv6 results, TCP/EDNS, suffix search, DNSSEC or a different retry policy can
use another resolver without changing coop's socket primitives. The wire limits and compression
rules come from [RFC 1035](https://www.rfc-editor.org/rfc/rfc1035.html); response association follows
the principles in [RFC 5452](https://www.rfc-editor.org/rfc/rfc5452.html).

## Tests

`coop_dns_parser_tests` exercises the production parser, hosts/config tokenizers and retry loop
without starting io_uring. It covers cyclic pointers, malformed prefixes, CRLF, name limits,
question/owner association, CNAMEs, truncation, borrowed config storage and error preservation.
`DnsRuntimeTest.*` in `coop_tests` exercises concurrent first use on one and multiple cooperators;
those tests require a native io_uring host and its normal localhost configuration.
