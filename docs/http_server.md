# HTTP server: caller-owned policy and borrowed parsing

`Connection<Transport>` parses requests incrementally over contiguous caller-sized buffers.
Its transport is a compile-time parameter; native plaintext and TLS instantiations are
provided. Include `coop/http/detail/server_impl.hpp` to instantiate a custom transport.
`ConnectionBase&` remains the handler boundary. The parser owns no request string, header map,
body accumulator, decoder, router, or retry mechanism.

## Body completion is explicit

```cpp
while (auto body = conn.NextBody()) {
    Consume(body->data, body->size); // borrowed; consume before the next parser operation
}
if (!conn.Complete()) {
    HandleReadFailure(conn.Error());
    return;
}
```

`BodyResult` truthiness means a span is available. Its terminal `Complete()` distinguishes
successful end of the request from `Error()` (negative errno). `ReadBody()` remains available;
its null result alone does not establish success. `Chunk::complete` marks the end of the
current element or wire chunk. A chunked request is complete only after its zero chunk,
trailers and final CRLF have been consumed.

`SkipBody()` is an explicit drain and returns whether the request completed. `Reset()` returns
false without IO for an incomplete, failed or closing request. A successful reset preserves
pipelined bytes and parser options. `KeepAlive()` reports persistence policy;
`Reusable()` additionally requires completed request consumption and no send failure. Neither
proves that caller-authored response framing is complete: the caller owns outgoing wire bytes.
The stock server drains after each handler and checks these returns before reusing a connection.

EOF before a declared body ends is `-ECONNRESET`; a receive timeout remains `-ETIMEDOUT`.
Errors stay observable and subsequent pulls do not keep receiving. A failure must not be
interpreted as a smaller successful upload.

## Borrowed lifetimes

Request method, version, path, query and target are views into receive storage. Copy or consume
what must survive subsequent parsing. `NextArgName()` tokenizes the raw query/target in place;
header/body parsing may compact or replace receive storage. The memoized request-line accessor
rejects reuse after either invalidation (asserting in debug). Returned argument/header names,
value chunks and body chunks are valid until the next parsing or reset operation. A body pull
never refills after creating the returned span, including at chunk delimiters.

Request lines and individual names must fit the selected receive buffer. Header values, chunk
extensions, trailers and bodies stream; enlarging any of these does not require an owning
representation or a larger receive buffer. Native construction requires a live context and
transport resource lifetime covering the connection. Connections are not copyable.

## Resource and response policy

Pass `ServerParserOptions` as the final connection constructor argument, or set it before
request-line parsing through `SetParserOptions()`. Stock listeners carry the same options in
`ServerConfiguration::parserOptions`. Options persist across requests.

| Option | Default | Meaning |
|---|---:|---|
| `maxHeaderCount` | 100 | Header plus trailer fields |
| `maxHeaderBytes` | 8192 | Field wire bytes, including colon, whitespace and CRLF; excludes request line and blank block delimiters |
| `maxChunkSize` | 1 GiB | Payload size of one wire chunk |
| `errorResponse` | `ParserErrorResponse::Automatic` | Who generates a parsing-error response |

`SIZE_MAX` permits all representable counts for a limit; zero permits zero. A body has no
implicit total-size limit. The application controls total admission and time policy. Limits
never relax framing validation or numeric overflow checks. Budget arithmetic does not wrap.

Automatic mode sends a 400/413/431 response when parsing fails before a response has begun.
If a response already began, the connection closes instead of inserting another status line.
`ParseError()` reports the HTTP status and `Error()` reports the underlying negative errno.
Transport and file-sink failures do not generate an automatic protocol-error response.

With `ParserErrorResponse::Caller`, the parser emits no error response. The handler may use
its normal response methods to render its own status, headers and body after inspecting
`ParseError()`. Reads and reuse remain disabled. If the handler already wrote part of a
response, it also owns the decision whether any further bytes are appropriate. The stock
listener dispatches malformed request-line errors to the handler in this mode; the handler
must tolerate a null `GetRequestLine()` and inspect `ParseError()`.

## Framing and file paths

Content-Length is parsed with checked arithmetic. Repeated or comma-joined lengths must agree.
Conflicting Content-Length and Transfer-Encoding are rejected. Transfer codings remain opaque;
a request transfer-coding list must end in exactly one `chunked`. The parser removes chunk
framing without decompressing preceding codings. Connection tokens are case-insensitive and
may span arbitrarily many receives. HTTP/1.0 defaults to close; HTTP/1.1 defaults to persistence.

`ReadBodyToFile()` retains the plaintext fixed-length socket-to-pipe-to-file splice path.
TLS, chunked and provided-buffer receive paths consume borrowed parser spans. It returns bytes
written or negative errno; a failed call may have written a prefix. Invalid offsets are
rejected; sink failures are terminal and do not masquerade as complete uploads.

`Sendfile()` validates ranges before flushing buffered headers and splits large counts into
transport-representable operations without copying file contents. A zero count performs no
file operation. Response components are literal caller-authored wire syntax: no escaping,
framing inference, compression, or hidden body buffering is installed.

## Deterministic validation

`coop_http_server_parser_tests` instantiates the production parser with a static scripted
transport. It covers every two-packet split and truncated prefix of fixed/chunked requests,
borrowed-span lifetime at chunk boundaries, numeric overflow, persistence tokens, explicit
limits, custom error responses, malformed grammar and large file-send counts. It does not
initialize io_uring. Native socket, splice, TLS and provided-buffer behavior remains covered
by the integration suite on a host that supports io_uring.
