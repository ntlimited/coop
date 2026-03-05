# coop/http/ — HTTP Server Internals

For the API surface (Route, Connection pull API, response methods, RunServer), see the
top-level `CLAUDE.md`. This file covers parser internals, buffer management, and performance
characteristics.

## Connection Buffer Management (`connection.cpp`)

`Connection<Transport>` is a contiguous bump allocation: the object and its recv buffer are one
allocation with `char m_buf[0]` as a trailing flexible member. Buffer size is configurable at
construction (default 2KB). The parser accesses `m_buf` via CRTP — `ConnectionImpl<Derived>`
calls `Buf()` which resolves to `static_cast<Derived*>(this)->m_buf`, a compile-time offset
from `this` with zero pointer indirection.

Parsing advances `m_parsePos` through the buffer; `Compact()` memmoves remaining data to the
front when more recv space is needed.

**Compact()**: `memmove(Buf(), Buf() + m_parsePos, remaining)` then resets `m_parsePos = 0`.
Guarded by `if (m_parsePos == 0) return` to skip no-op calls. Called before `RecvMore()` in
scan loops when data spans the buffer boundary.

**Design note**: `Compact()` calls should only appear in "need more data" branches — the
boundary-spanning case. For typical HTTP requests (200-500 bytes), the entire request fits
in one recv and Compact never fires during parsing. Avoid adding Compact calls in paths where
`m_parsePos == m_bufLen` (the memmove would be zero-length); `RecvMore()` handles compaction
internally when the buffer is full.

## Parser Phases

Strictly sequential: `REQUEST_LINE` -> `ARGS` -> `HEADERS` -> `BODY` -> `DONE`. Each phase
implicitly skips the previous if not consumed (`AdvanceToPhase`). The phase enum gates access:
calling `NextHeaderName()` before consuming args auto-advances through `SkipArgs()` ->
`SkipToHeaders()`.

**Special header detection**: Content-Length, Transfer-Encoding, and Connection headers are
detected during header parsing. `NextHeaderName()` sets pending flags
(`m_pendingContentLength`, etc.); `ReadHeaderValue()` captures the values. `SkipHeaderValue()`
delegates to `ReadHeaderValue()` for special headers to ensure they're captured even when the
handler doesn't read them. `SkipHeaders()` scans all header lines and calls
`DetectSpecialHeader()` directly.

## Response Formatting

`Send()`, `BeginChunked()`, `SendChunk()`, `EndChunked()` use `snprintf` to format HTTP
response headers into a stack buffer. This shows up in perf profiles as `__vfprintf_internal`
(~0.5% under load) — it's legitimate hot-path work, not logging.

## Keep-Alive

`Reset()` reinitializes parser state between requests on the same connection. It calls
`Compact()` first to preserve any leftover pipelined data in the buffer, then zeroes all
parser state. `SkipBody()` must be called before `Reset()` to drain unconsumed body bytes.

## Performance Profile (perf observations)

Under wrk load, the HTTP server is **overwhelmingly kernel-bound**. Top userspace symbols:
- `Cooperator::DrainSubmissions` ~2% (per-connection context spawning)
- `snprintf` ~0.5% (response header formatting)
- `Uring::Poll` ~0.3%
- `Connection::SkipHeaders` ~0.1%
- `ContextSwitch` ~0.1%

The TCP stack (recv/send/ack/skb alloc) dominates. The HTTP framework and cooperator are
nearly invisible in profiles.

## Benchmark Infrastructure

- `benchmarks/bench_http.cpp`: Google Benchmark microbenchmarks. Tests parse+respond latency
  per iteration over unix socket and TCP loopback. Includes minimal, realistic (args+headers),
  and response-size-scaling scenarios. Always run in release mode.
- `benchmarks/bench_server.cpp`: Standalone HTTP server for external load testing with wrk.
  Supports `--sqpoll` flag for SQPOLL mode. Usage: `bench_server [port] [--sqpoll]`
