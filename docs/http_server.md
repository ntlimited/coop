# HTTP server: caller-owned policy and borrowed parsing

`Connection<Transport>` parses requests incrementally over contiguous caller-sized buffers.
Its transport is a compile-time parameter; native plaintext and TLS instantiations are
provided. Include `coop/http/detail/server_impl.hpp` to instantiate a custom transport.
`ConnectionBase&` remains the handler boundary. Ordinary parsing owns no request string or
header map. Request-head inspection explicitly opts into a bounded snapshot; neither path
owns a body accumulator, decoder, router, or retry mechanism.

## Request-head inspection and admission

`const RequestHead* ConnectionBase::InspectRequestHead(size_t maxBytes = 16384)` gives
handlers stable borrowed method, version, path, query, target and header views before they
consume query arguments or headers. `GetRequestLine()` may precede inspection, but copied
request-line views can be invalidated by capture; re-fetch it afterwards. A first call
after iteration has begun returns null without rewinding or changing parser state. A successful
snapshot can be read again throughout the request, including after body parsing. The cap applies
to the first capture; cached calls do not recapture or change it. Its views
expire at `ReleaseRequestHead()`, the next successful `Reset()`, or connection destruction.
`void ConnectionBase::ReleaseRequestHead()` invalidates every snapshot view and releases
its storage. It defaults to a no-op for adapters. After release, inspection returns null until
Reset. Explicit handler inspection retains its snapshot until one of those lifetime boundaries.

`RequestHead::headers` is a `std::span<const RequestHeader>` in wire order. Each field has
`std::string_view name` and `value`; duplicate fields remain separate. Values omit leading
and trailing optional whitespace. `const RequestHeader* RequestHead::FindHeader(
std::string_view name) const` performs ASCII case-insensitive lookup of the first field and
returns null for absence, distinct from an empty value. It is first-only, not a uniqueness check.
`size_t RequestHead::CountHeaders(std::string_view name) const` counts matching fields;
`const RequestHeader* RequestHead::FindUniqueHeader(std::string_view name) const` returns
null for absence or duplicates. Both use ASCII case-insensitive matching. Security policies
should use unique lookup or explicit duplicate handling through the span.

Stock listeners can perform this check before every dispatch, including a handler's calls
to the library's status, performance and sampler dispatcher:

```cpp
config.requestAdmission = [](http::ConnectionBase&, const http::RequestHead& head,
                             void*) -> int {
    const auto* host = head.FindUniqueHeader("Host");
    const auto* origin = head.FindUniqueHeader("Origin");
    if (!host || host->value != "allowed.test" || !origin ||
        origin->value != "https://allowed.test") return 403;
    return 0;
};
config.maxRequestHeadBytes = 16384;
```

The callback type is `int (*RequestAdmissionHandler)(ConnectionBase&, const RequestHead&,
void*)`. It runs on the serving context and receives the server's `userData`. Zero accepts;
400 through 599 refuse with that status and close; any other return value refuses with 500.
The hook applies independently to every keep-alive or pipelined request on plaintext and TLS.
It can yield and use connection state. The server releases the snapshot immediately after
this callback returns; route handlers use ordinary request-line/header parsing afterwards.
No head capture, allocation or additional header
parsing occurs when neither a hook nor a handler requests inspection.

Inspection validates the complete head through the ordinary request parser before returning
it. Normal header parsing rejects missing HTTP/1.1 Host fields and duplicate Host fields
with 400 ([RFC 9112 section 3.2](https://www.rfc-editor.org/rfc/rfc9112.html#section-3.2)). Admission cannot approve one Host while routing sees another.
The target remains the same verbatim request-target in both interfaces; absolute-form targets
are not rewritten into a different authority or path. Malformed framing receives 400, and either a parser-header budget or inspection-wire cap
violation receives 431, before admission or route dispatch. Inspection owns newly encountered
protocol-error responses even with Caller parser policy. Existing errors remain unchanged.
Body bytes are never consumed or interpreted. Network receives may have prefetched body or
pipelined bytes as with ordinary parsing; those bytes remain in their original receive window.
Normal argument, header and body iteration then replays the validated head without losing or
duplicating bytes. Provided-buffer windows follow the same contract.

The explicit cap covers request-line and header wire bytes together. Snapshot workspace has
two byte buffers, each bounded by the selected cap, plus at most `floor(maxBytes / 4)` header
index entries of `sizeof(RequestHeader)` bytes (also bounded by `maxHeaderCount`). Capture grows
geometrically with actual input instead of reserving the cap. After admission, snapshot/index
storage is released immediately and the parser's replay copy is released at header completion.
Thus an idle keep-alive connection retains no dynamic head-inspection storage: only the fixed
vector/view bookkeeping remains in the connection object. Explicit handler inspection must call
ReleaseRequestHead to obtain the same lifetime. The snapshot never stores the body. Set parser policy before inspection; changing it afterwards
is refused. Allocation failure refuses with 503. The default interface implementation returns
null for connection adapters that do not support inspection.

`bool ConnectionBase::RejectRequest(int status, bool responseStarted = false)` provides the
same terminal refusal to a handler. It forces closure and sends a status with a fixed-length text body. If a response
already began, it adds no bytes and only marks the request refused, allowing a caller to
format custom headers first. Pass `responseStarted = true` after custom formatting so the
default connection-adapter implementation also omits an additional response. Invalid status
codes become 500. Repeated calls add no bytes;
the return value reports response-send success. `bool ConnectionBase::RequestRejected() const`
reports the native terminal state. Refusal preserves an existing error; otherwise `Error()`
becomes `-ECONNABORTED`, and `Complete()`/reuse remain false. The stock server bypasses its
normal post-handler body drain and uses the bounded refusal cleanup described below. Custom
adapters inherit a conservative close-and-send fallback and may implement the full terminal
contract.

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

## Endpoints without request bodies

A handler or its dispatcher can opt into body-free requests with one call before application
work or response generation:

```cpp
if (!conn.RequireNoBody()) return;
HandleStatusRequest(conn);
```

`bool ConnectionBase::RequireNoBody()` consumes remaining query arguments and headers,
without reading body bytes. Consume any needed query arguments first. It accepts absent or
zero Content-Length. A nonzero Content-Length or any Transfer-Encoding refuses the request
with 400, disables subsequent sends and reuse, and makes the stock listener close the
connection. This admission call owns newly encountered protocol-error responses even with Caller
parser-error policy: malformed framing receives 400, and header-budget errors receive 431.
The configured parser policy is restored after the call. Errors encountered before admission
remain unchanged. A response already started cannot be replaced,
so call the guard before sending anything. False can also indicate an existing parse or IO
failure; inspect `Error()` and `ParseError()` as usual.

The built-in status, performance, sampler, health, cooperator, epoch and context endpoints
apply this check before their action handlers run. Their query options are captured first.
The symbolization endpoint accepts its documented request body. The standalone status
server also checks static-file and not-found requests. Chunk payload validity is irrelevant
to admission: even a chunked request containing an invalid first chunk is refused before
its target handler runs.

The stock server half-closes its write side after a protocol-error or explicit refusal response,
then discards up to 1 MiB of unread socket data for at most 200 ms. Kill and the serving context's deadline
can end that drain sooner. Provided-buffer receives are stopped before the drain. On giving
up, the server shuts down reads and discards one snapshot of queued input with a nonwaiting,
copy-free receive. This prevents the unread queue from aborting the already-sent response
when the socket closes, without extending the drain for a continuing uploader. Uploads beyond
either bound are cut short; clients should read the final response even when their upload
fails. No body bytes reach the rejected handler.

The default `ConnectionBase::RequireNoBody()` implementation refuses without reading and
forces connection close. Custom connection implementations can override it to provide the
framing check; the native implementation sends the documented protocol-error response.

## Borrowed lifetimes

Request method, version, path, query and target are views into receive storage. Copy or consume
what must survive subsequent parsing. `NextArgName()` tokenizes the raw query/target in place;
header/body parsing may compact or replace receive storage. The memoized request-line accessor
rejects reuse after either invalidation (asserting in debug). Returned argument/header names,
value chunks and body chunks are valid until the next parsing or reset operation. A body pull
never refills after creating the returned span, including at chunk delimiters.

Header values omit leading and trailing SP/HTAB in both `ReadHeaderValue()` and request-head
inspection. Interior whitespace is preserved exactly. Framing checks and `maxHeaderBytes`
charge the original wire bytes, including omitted whitespace. A possible trailing whitespace
run spanning receive windows is temporarily retained, bounded by `maxHeaderBytes`; setting
that policy to `SIZE_MAX` explicitly removes the bound. This scratch storage is released when
the run is discarded or after its returned interior-whitespace view expires. The scratch adds only
an empty vector and a flag to idle connections, with no additional heap storage.

Request lines and individual names must fit the selected receive buffer. Header values, chunk
extensions, trailers and bodies stream without requiring a larger receive buffer. Native construction requires a live context and
transport resource lifetime covering the connection. Connections are not copyable.

## Resource and response policy

Pass `ServerParserOptions` after the connection timeout constructor argument, or set it before
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

`ContentLength()` finishes the entire header block before reporting a validated advertised
length: `0` means explicit zero, and `-1` means no Content-Length or a parsing failure.
Check `Error()` to distinguish absence from failure. Repeated queries do not change framing
or consume body bytes. Consumers that treated an absent length as zero must handle `-1`.
Under [RFC 9112 section 6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3), a request
without Content-Length or Transfer-Encoding has no body; a response without either is
close-delimited unless its method or status forbids a body.

Content-Length is parsed with checked arithmetic. Repeated or comma-joined lengths must agree.
Content-Length together with any Transfer-Encoding field is rejected, including an empty field.
Transfer codings remain opaque; the combined request transfer-coding list must be nonempty and
end in exactly one `chunked`. Empty list members are ignored. The parser removes chunk
framing without decompressing preceding codings. Connection tokens are case-insensitive and
may span arbitrarily many receives. HTTP/1.0 defaults to closure; an explicit
`Connection: keep-alive` permits persistence for Content-Length responses. Consume needed query
arguments and call `SkipHeaders()` before sending to honor that request: response methods do
not parse unread headers. An early HTTP/1.0 response selects closure rather than promising
persistence it has not learned from the request. HTTP/1.1 defaults to persistence. `std::string_view ConnectionBase::GetRequestVersion() const` returns a stable
protocol literal after parsing, independent of receive-buffer movement, so handlers can choose
fixed-length framing for HTTP/1.0. Native connections return an empty view before parsing;
custom adapters inherit an HTTP/1.1 default.

The retained version selects every response status line. Fixed responses retain Content-Length.
`BeginChunked`/`SendChunk`/`EndChunked` on HTTP/1.0 force closure and stream the body without
chunk markers or a terminator. A successful `EndChunked` marks the stream complete. The stock
TLS server then flushes its close_notify alert within the connection timeout, capped at one
second, without waiting for a peer alert. Completion remains valid if the context is killed
or its deadline expires afterward: plaintext retains a clean FIN, and TLS gets one nonwaiting
closure-alert attempt. An alert that cannot be sent still lets TLS detect truncation, but
does not turn the completed body into an abortive TCP reset. Incomplete streams receive no TLS
closure alert and use an abortive TCP reset so a truncated close-delimited body cannot look
complete. Component responses that attempt Transfer-Encoding or Trailer, and all 1xx
responses on HTTP/1.0, fail permanently before sending further bytes.

`int io::ssl::Connection::ShutdownWriteKill(time::Interval timeout)` sends only the local TLS
closure alert, observes cancellation and deadlines, and returns zero on success or negative
errno. A nonpositive timeout uses a one-second bound. It does not close the descriptor or wait
for the peer's closure alert. The caller must close the transport after success or failure.

`ReadBodyToFile()` retains the plaintext fixed-length socket-to-pipe-to-file splice path.
TLS, chunked and provided-buffer receive paths consume borrowed parser spans. It returns bytes
written or negative errno; a failed call may have written a prefix. Invalid offsets are
rejected; sink failures are terminal and do not masquerade as complete uploads. Body reads,
file writes, splice waits and fairness yields use the context running the read, including
a separate child reader. Kill or an expired deadline stops that reader even when its
construction context remains healthy.

`Sendfile()` validates ranges before flushing buffered headers and splits large counts into
writes of at most 16 KiB. Plaintext and kTLS use pooled pipes and forced-async io_uring
splice, preserving zero-copy while source page faults run outside the scheduler thread.
Other TLS file sends stage at most 16 KiB through cooperative file reads and TLS writes.
Untimed IO sendfile APIs retain their existing paths. A zero count performs no file operation. Response components are literal caller-authored wire syntax: no escaping,
framing inference, compression, or hidden body buffering is installed.

## Send progress and cancellation

The connection timeout bounds socket-write inactivity as well as receives. Each positive
write refreshes the inactivity budget, so a steady transfer may last longer than the timeout.
The sending context's absolute deadline remains a cap on the whole operation.
There is no absolute cap by default: a reader accepting even a few bytes per interval can
keep a transfer alive indefinitely. Applications needing a total lifetime bound should call
`ctx->SetDeadlineIn(...)` on the serving context before sending, and set a deadline on any
separate writer context they spawn. Kill and expired deadlines also stop continuously writable sends. After cancellation, one immediate,
nonwaiting write may deliver a small final response; it succeeds only if every byte in that
write is accepted. A partial or blocked final write terminates the stream. The allowance is
once per connection, including when a different context subsequently uses it. Buffered header/final chunk flushes,
raw writes and upgrades use the same policy. An oversized canceled buffer is refused before
any new bytes are written. File sends cannot use the final attempt; cancellation before
flushing buffered response bytes or entering the first file transport call returns false without marking the connection failed, leaving
the allowance available for a later small raw write. This refusal leaves `Error()` unchanged.
A send that has entered the transport and fails is terminal;
HTTP exposes its negative errno through `Error()` and returns false from subsequent sends.
Timeouts report `-ETIMEDOUT`; cancellation reports `-ECANCELED` unless the one immediate final
write succeeds. A timed TLS write or closure-alert failure also poisons the public TLS write
API: later `Send` and `Sendfile` variants return `-EPIPE`. After a successful immediate final
data write completes a close-delimited stream, its owner may still send the TLS closure
alert. An already canceled or expired owner gets one nonwaiting alert attempt; a healthy
owner uses the bounded alert operation. Neither resumes application writes nor recovers
a failed TLS record.

Custom server transports provide these synchronous cooperative progress operations:

```cpp
int Send(const void* data, size_t size, coop::time::Interval timeout);
int TrySend(const void* data, size_t size); // optional, one nonwaiting final attempt
int Sendfile(int fileFd, off_t offset, size_t count, coop::time::Interval timeout);
```

A positive result is the actual byte count transferred, which may be shorter than requested;
zero means the stream or file ended, and negative values are errno-style failures. The parser
requests at most 16 KiB per call. A positive timeout bounds inactivity, while zero imposes no
inactivity limit. Every nested wait must observe kill and the context deadline. `TrySend` must never wait or yield; transports omitting it refuse final writes after
cancellation. Native transports retain `SendAll`/`SendfileAll` for the HTTP client API. Repeated successful server
sends yield after each 2 MiB of progress, including progress spread over many response calls.
No payload allocation or retained caller buffer is introduced.

## Deterministic validation

`coop_http_server_parser_tests` instantiates the production parser with a static scripted
transport. It covers every two-packet split and truncated prefix of fixed/chunked requests,
borrowed-span lifetime at chunk boundaries, numeric overflow, persistence tokens, explicit
limits, custom error responses, malformed grammar and large file-send counts. It does not
initialize io_uring. Native socket, splice, TLS and provided-buffer behavior remains covered
by the integration suite on a host that supports io_uring.

## Application connection state

`ServerConfiguration::connectionOpen` and `connectionClose` are optional callbacks:

```cpp
using ConnectionOpenHandler = bool (*)(Context*, const sockaddr* peer, socklen_t peerLength,
                                       void* userData, void*& state);
using ConnectionCloseHandler = void (*)(Context*, void* userData, void* state);
```

Open runs on the serving context before request parsing or a TLS handshake. The peer socket
address is borrowed only during the callback; copy it to retain it. Address-query failure
supplies nullptr and a zero length. The output state is
borrowed through `void* ConnectionBase::GetConnectionState() const` for every request on
that connection. The existing handler `userData` remains per-server state. Directly constructed
connections can pass an optional state pointer as the last constructor argument.
`ConnectionBase::GetConnectionState()` supplies a null default for custom connection interfaces.
The open hook can call `ctx->SetDeadlineIn(...)` to cap the connection's overall lifetime,
including sends that continue making progress. Separate writer contexts need their own bound.

Returning false refuses only that connection. Close runs exactly once when configured,
including refusal, EOF, kill, and TLS handshake failure; it receives the state output even
when open returned false. With no open callback, state is null. Both hooks may cooperate.
The application allocates, accounts for, and frees its state; the library owns no application
allocator.

Close follows the last handler or upgraded protocol handler and retirement of all attached
child context trees, including their cleanup. Connection resources remain live through that
join. `Context::JoinChildren(KillCause cancel = KillCause::None)` provides that lifetime
boundary: the default joins, while an explicit cause cancels attached descendants first.
The server cancels children on exit while preserving a healthy serving context's live state;
its close hook can still cooperate. Existing cancellation and deadlines propagate normally.
Like Context retirement, the join cooperatively waits for cleanup to finish even when that
cleanup ignores cancellation; abandoning it would free borrowed state early. The server uses
this boundary before parser and transport teardown even when connection hooks are unset.
A detached child must arrange its own state lifetime. Hooks and per-server state must
outlive active connections; when graceful draining detaches connections from the listener,
wait for drain completion before destroying them.

Response bodies are borrowed for the duration of `Send`. The virtual operation is
`bool Send(int status, const char* contentType, std::span<const std::byte> body)`.
Text callers use a constrained nonvirtual adapter for values convertible to
`std::string_view`; the pointer/size overload remains a non-owning adapter.
A `nullptr` body literal is refused at compile time, a null C-string pointer
returns false without sending, and `{}` selects an empty byte span. Neither adapter allocates or retains the
body. Custom connection implementations override only the byte-span operation
and expose the base overloads with `using ConnectionBase::Send` when calling them
through the concrete type. Existing owning strings can be passed as text views;
embedded NUL bytes are preserved by their explicit length.
