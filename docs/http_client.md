# Native HTTP client

`ClientConnection<Transport>` frames HTTP/1.x over a connection the caller supplies.
The caller owns connection establishment, admission, request sequencing, authentication,
timeouts/cancellation, retries, and disposal. The client stores no owning header map or body
and launches no background work. Parsing is synchronous-looking cooperative I/O: a pull
may receive more data, but only the caller's pulls advance the response.

## Construction and ownership

```cpp
using Client = coop::http::ClientConnection<coop::http::PlaintextTransport>;
coop::http::PlaintextTransport transport(descriptor, coop::http::RecvPolicy::PollFirst);
auto client = ctx->Allocate<Client>(Client::ExtraBytes(), transport, "origin.example:8080");
```

The object and its receive/send buffers are one contiguous allocation. For custom buffer
sizes, pass the same sizes to `ExtraBytes(recvSize, sendSize)` and the constructor. The
receive buffer must be at least two bytes and the send buffer nonempty. Status lines and
header names must fit the caller's receive buffer; header values, bodies, chunk extensions,
and trailer lines stream across it. A contiguous token exceeding that buffer reports
`-EMSGSIZE`. There are no additional fixed header-count or body-size policy limits.

The descriptor, TLS connection when used, and host string must outlive the client. The host
string is the literal Host field value, including an explicit port when needed. Transports
borrow their resources; constructing or destroying the HTTP client does not close a socket.
The client cannot be copied or moved because its trailing buffers and parser views belong
to that allocation. Context allocations must unwind in LIFO order.

## Checked body consumption

```cpp
if (!client->Get("/object")) return false;
int status;
while (true)
{
    auto* line = client->GetResponseLine();
    if (!line) return false;
    status = line->status; // retain scalars before advancing borrowed views
    if (status >= 200) break;
    if (status == 101) return false; // this caller does not accept upgrades
    if (!client->SkipBody() || !client->AdvanceResponse()) return false;
}

while (true)
{
    auto body = client->NextBody();
    if (!body)
    {
        if (!body.Complete()) return HandleFailure(body.Error());
        break;
    }
    Process(body->data, body->size);
}
if (client->Reusable())
{
    if (!client->Reset()) return false;
    // The caller may now send another request on this connection.
}
```

`BodyResult` is a trivially copyable pointer/status flyweight. Truth means a data span is
available; a false result distinguishes successful end through `Complete()` from a negative
errno through `Error()`. It owns no bytes. `ReadBody()` retains the pointer-only interface;
callers using it must check the connection's `Complete()` or `Error()` after the loop.
`SkipBody()` explicitly drains the remaining response and returns whether it completed.
Destroy the connection's owning descriptor to abandon a body without draining it.

| Value | Validity / meaning |
|---|---|
| Response status/reason | Copy the status scalar; the reason borrows receive storage. |
| Header name | NUL-terminated borrowed name; use or copy before reading its value. |
| Header value/body span | Borrowed until the next parsing/reset operation; no refill occurs after forming the returned view. |
| `Chunk::complete` | End of the current element; for chunked bodies this is one wire chunk, not the response. |
| `BodyResult::Complete()` | Successful end returned by this pull, after all body framing and trailers. |
| `client->Complete()` | Current response fully consumed without failure. |
| `KeepAlive()` | Peer persistence policy, false after failure; not proof of completion. |
| `Reusable()` | Complete final response on a persistent HTTP connection. |

`Reset()` returns false for incomplete, failed, close-delimited, and upgraded connections.
It never drains input, erases failure, reconnects, or retries. A transport/parser failure is
terminal for this HTTP client. `ContentLength()` parses headers if necessary, reports the
advertised entity length, and remains `-1` when none was declared; it never changes framing.
Successful CONNECT ignores Content-Length/Transfer-Encoding and reports length `-1`.

## Framing and caller policy

Fixed-length, chunked, and close-delimited responses are supported. Content-Length must
contain valid nonnegative decimal values that fit `int64_t`; repeated values must agree.
Conflicting Content-Length/Transfer-Encoding on body-bearing responses is rejected, as is
a Transfer-Encoding list containing no coding. Empty list members are ignored when the
combined field values contain a coding. Chunk sizes are checked for overflow, delimiters
and extensions validated, and trailers consumed before completion.
Trailers are validated/discarded without materializing them or changing body framing;
this interface does not expose trailer fields.

HEAD, 1xx, 204 and 304 have no body. Informational responses are exposed to the caller:
consume their headers/body, then call `AdvanceResponse()` to read the next response for
the same request. It preserves HEAD/CONNECT method semantics and sends nothing. Successful
CONNECT and status 101 end HTTP framing and disable HTTP reuse; protocol takeover is not
a full tunnel/upgrade API, including handoff of buffered bytes.

Transfer codings other than chunked remain opaque. For `gzip, chunked`, the client removes
chunk framing and exposes gzip bytes; a final non-chunked coding is close-delimited.
Content-Encoding is also left untouched. The caller decides whether to decode or reject.

`SendRequest` emits Content-Length for a supplied body independently of Content-Type.
The component API (`BeginRequest`, `AppendHeader`, `EndHeaders`, `SendBody`) leaves request
framing with the caller: `SendBody` writes raw bytes, including caller-built chunk framing
when Transfer-Encoding is used. It performs no escaping, signing, compression, or retry.
Inputs to the request builder are already validated wire components supplied by the caller.

`ReadBodyToFile` and `SendBodyFromFile` are explicit disk-transfer operations. The receive
path preserves plaintext splice for framed bodies and uses parser-buffer writes for TLS,
chunked, close-delimited, or provided-buffer input. Errors remain negative errno values.

## Validation and transport boundaries

`coop_http_parser_tests` runs the production parser with a static scripted transport. It
covers receive splits, single-byte reads, every truncated prefix, borrowed-view lifetimes,
malformed framing, explicit completion/reuse, and outgoing request formatting without
initializing io_uring. Native socket, provided-buffer, and file paths remain covered by the
io_uring integration suite on a capable host.

The constructor timeout is a per-receive interval on plaintext transports, not a transaction
deadline. `TlsTransport` currently does not apply it; send/handshake cancellation and total
deadlines must be composed at the transport/runtime layer. These HTTP framing contracts do
not add a timer, kill hook, or cancellation policy to each connection.

`bench_http_parser` isolates parsing CPU from kernel/socket cost, with contiguous and
fragmented fixed-length/chunked responses. Build in Release and pin the process for
comparisons. Validation examines bytes the older permissive parser skipped; its CPU cost
must be measured separately from network throughput. No socket performance claim follows
from the parser-only numbers.
