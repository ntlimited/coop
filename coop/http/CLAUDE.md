# coop/http/ — HTTP Internals

For the API surface (Route, Connection pull API, response methods, RunServer), see the
top-level `CLAUDE.md`. This file covers parser internals, buffer management, the client,
and performance characteristics.

## Connection Buffer Management (`connection.cpp`)

`Connection<Transport>` is a contiguous bump allocation with dual trailing buffers:
`[Connection<T> fields] [recv buffer ... recvBufSize] [send buffer ... sendBufSize]`.
Both are part of one `char m_buf[0]` trailing flexible member. Default recv: 2KB, send: 512B.

The parser accesses the recv buffer via CRTP — `ConnectionImpl<Derived>` calls `RecvBuf()`
which resolves to `static_cast<Derived*>(this)->m_buf`, a compile-time offset. The send buffer
is accessed via `SendBuf()` = `m_buf + m_recvBufSize`.

Allocation: `ctx->Allocate<Connection<T>>(recvBufSize + sendBufSize, transport, ctx, co,
recvBufSize, sendBufSize)`.

Parsing advances `m_parsePos` through the recv buffer; `Compact()` memmoves remaining data to
the front when more recv space is needed.

**Compact()**: `memmove(RecvBuf(), RecvBuf() + m_parsePos, remaining)` then resets
`m_parsePos = 0`. Guarded by `if (m_parsePos == 0) return` to skip no-op calls. Called before
`RecvMore()` in scan loops when data spans the buffer boundary.

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

Response methods use a write buffer (`m_sendLen` tracks fill level in the send buffer) with
pre-compiled constants (`response_constants.h`). Status lines, header names, and connection
trailers are pre-built `Fragment`s — memcpy'd via `Append`/`AppendLiteral`. Numeric values
(Content-Length, chunk sizes) use hand-rolled `AppendUInt`/`AppendHex` (no snprintf). The
write buffer flushes automatically on overflow or explicitly via `Flush()`.

Status lines outside the pre-compiled table are formatted at runtime by `AppendStatusLine`:
`HTTP/1.1 <code> <reason>` where the reason is the caller's (proxy passthrough of the
upstream phrase) or `response::DefaultReason(code)`'s status-class fallback. All response
methods route through it, so any 3-digit code is sendable.

## Component Response API (proxying)

`BeginResponse(status, reason)` / `AppendHeader(name, value)` / `EndHeaders()` emit the
response in parts for responses the composed methods can't express — arbitrary header sets
(Content-Range, ETag, x-amz-*), forwarded reason phrases. `EndHeaders` appends the
framework's Connection header and flushes. Body framing is the caller's job: append
Content-Length and stream via `SendRawBytes`, append `Transfer-Encoding: chunked` and use
`SendChunk`/`EndChunked` (they skip their deferred-header path when `BeginChunked` wasn't
used), or `ForceClose()` for EOF-framed passthrough. A proxy must not forward the upstream
`Connection` header — `EndHeaders` owns it.

`RequestLine::target`/`query` carry the raw request-target (path + query, verbatim) for
upstream forwarding; `path` remains the pre-`?` slice. Views dangle once parsing advances —
copy before consuming headers.

## Disk Data Paths

`ReadBodyToFile(fileFd, offset)` (server and client) is the receive-to-disk idiom: body bytes
already pulled into the recv buffer during header parsing are written first (through the
ring), then the framed remainder moves socket -> pipe -> page cache via `io::SpliceToFile`
with a pool-leased pipe — no userspace visit. The splice length is bounded by the body's
framing, so pipelined bytes behind the body stay in the socket. Transport spliceability is a
compile-time property (`Transport::kSpliceable`: plaintext yes, TLS no — decryption must
visit userspace); TLS and chunked bodies bounce through the parser buffer instead, same
contract, just copied. On success the parser lands in DONE, positioned for keep-alive Reset.
Client side treats HEAD/204/304 as framing-only (returns 0).

Serve-from-disk was already first-class: `Sendfile` on the server (kTLS-aware via the
transport), and now `SendBodyFromFile` on the client (PUT of a cached object — flush headers,
then sendfile through the transport). Splice and sendfile need non-blocking sockets; accepted
server sockets and anything from `io::Connect` should already be `O_NONBLOCK`.

For small responses (headers + body fit in 512B), the entire response coalesces in the send
buffer and goes out in one `SendAll` syscall. Large bodies flush headers first, then send the
body directly via `SendRaw`. Chunked encoding accumulates hex size + data + CRLF in the buffer,
coalescing multiple small chunks before flush.

The `WritevAll` / iovec pattern is eliminated — both `PlaintextTransport` and `TlsTransport`
no longer provide `WritevAll`. TLS benefits especially: the buffer replaces the deferred-send
coalescing that `WritevAll` previously handled.

## Keep-Alive

`Reset()` reinitializes parser state between requests on the same connection. It calls
`Compact()` first to preserve any leftover pipelined data in the buffer, then zeroes all
parser state. `SkipBody()` must be called before `Reset()` to drain unconsumed body bytes.

## Client (`client.{h,cpp}`)

`ClientConnection<Transport>` mirrors the server connection: CRTP parser, trailing dual
buffers, phases `RESPONSE_LINE -> HEADERS -> BODY -> DONE`. Composed requests: `Get`,
`Post`, `Head`, `SendRequest`. Component requests for arbitrary headers and streamed
bodies: `BeginRequest(method, path)` (request line + Host, and remembers HEAD) /
`AppendHeader(name, value)` / `EndHeaders()` (blank line + flush) / `SendBody(data, size)`
(append + flush; oversized bodies bypass the buffer). Caller owns body framing via a
Content-Length or Transfer-Encoding header.

Response bodies: `ReadBody` handles Content-Length and chunked. HEAD responses and 204/304
statuses are framing-only — `ReadBody` ends immediately while `ContentLength()` still
reports the advertised entity length (a proxy forwards it), keeping the connection
positioned for keep-alive reuse.

## Performance Profile (perf observations)

Under wrk load, the HTTP server is **overwhelmingly kernel-bound**. Top userspace symbols:
- `Cooperator::DrainSubmissions` ~2% (per-connection context spawning)
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
- `benchmarks/bench_disk_path.cpp`: Standalone receive-to-disk engine A/B (SpliceToFile vs
  recv+write bounce) over TCP loopback. Run in release, pinned. Result and interpretation
  recorded in `docs/zero_copy_survey_2026-08.md`.
