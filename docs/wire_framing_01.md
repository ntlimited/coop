# Wire Framing 01

This note is the framing contract for coop's HTTP server parser and its WebSocket
frame parser: what a request or a frame must say about its own shape before its
bytes are handed to a handler, and what happens when it says something else.

## Problem

Both parsers read a stream whose every field is written by the other end. A listener
built on them is typically bound to all interfaces with no authentication in front,
and an HTTP connection runs its parser on a 32KB coroutine stack. So the numbers a
request supplies — a chunk size, a Content-Length, a WebSocket payload length — are
not measurements of anything. They are claims, and the parser is the only thing that
decides whether to believe them.

Framing is where a wrong belief is expensive, because framing decides where one
message ends and the next begins. A parser that computes a different end than the
sender intended does not merely mis-read one request: the bytes past the end it
chose become the beginning of a request nobody sent, delivered to the handler with
the same standing as a real one. The same is true across hops — an intermediary and
an origin that compute different boundaries for the same bytes is the classic
request-smuggling primitive, and the disagreement is always some field one of them
read loosely.

Loose reading takes a few recognizable shapes:

- **Decoding a field as something other than what it claims to be.** A hex chunk
  size decoded with `c & 0xF` accepts every byte and reads 'a'-'f' as 1-6. A
  decimal Content-Length parsed by skipping non-digits reads `1abc0` as 10.
- **Treating a delimiter that has not arrived as if it had.** A header line ends
  with CRLF. When only the CR is buffered, stopping there leaves the parser on the
  CR, and the next scan reads that CR and the LF behind it as the blank line that
  ends the header block — hiding every remaining header, Content-Length included,
  and leaving them to be parsed as a second request.
- **Skipping a delimiter without reading it.** Stepping over the two bytes after a
  chunk's payload, or over a trailer section, without checking them lets the peer
  choose what is there.
- **Accepting two answers to one question.** Content-Length and Transfer-Encoding
  both say where the body ends; two Content-Lengths with different values say it
  twice, differently.
- **Letting a pointer into the receive buffer outlive a refill.** The buffer
  compacts and refills in place. A view handed to a caller across that boundary, or
  a `strlen` over a name whose terminator moved, reads bytes the next `recv` wrote.
- **Trusting a size to be a size.** A 64-bit WebSocket length with its top bit set,
  or a payload larger than any buffer, is not a number to allocate or loop against.

## Decision

**A framing field is validated as the exact grammar it claims to be, and the
connection is failed otherwise.** Not repaired, not guessed at, not partially
honored — failed.

For HTTP (`coop/http/connection.{h,cpp}`):

- `chunk-size` is `1*HEXDIG`, at most 16 digits, optionally followed by `;` and
  chunk extensions, and terminated by CRLF. A value above `MAX_CHUNK_SIZE` is
  refused. Anything else is `400`, or `413` for the ceiling.
- Each chunk's data is followed by its own CRLF, which is read, not skipped. The
  terminal chunk is followed by the trailer section, which is consumed through its
  blank line so that nothing of this message is left to be read as the next one.
- `Content-Length` is `1*DIGIT`, at most 19 digits, decoded whole even when the
  value spans a refill. Leading zeros are digits and are accepted. Repeating the
  header is allowed only when it repeats the same value.
- `Content-Length` together with `Transfer-Encoding` is refused rather than
  resolved in either direction (RFC 7230 3.3.3). The only transfer coding that can
  be framed is `chunked`, matched as a whole token.
- A line terminator is CRLF with both bytes present. A lone buffered CR is not a
  terminator; the parser waits for the LF and fails if it cannot get it.
- The header block is bounded by `MAX_HEADER_COUNT` lines and `MAX_HEADER_BYTES`
  cumulative bytes, both charged across refills and shared with trailer lines. The
  recv buffer bounds one line; it does not bound a block that compacts as it scans.
  Exceeding either is `431`.
- A failed request is terminal. `ConnectionBase::ParseError()` reports the status,
  the peer is answered once, keep-alive is off, response calls from the handler are
  refused, and `Reset()` is a no-op — the buffered bytes are never re-read as the
  next request.

For WebSocket (`coop/ws/connection.{h,cpp}`, `coop/ws/upgrade.cpp`):

- RSV1-3 must be zero (no extension is negotiated); opcodes outside
  `{0,1,2,8,9,10}` are refused; a client-to-server frame must be masked. Each is
  close `1002`.
- A control frame carries at most `MAX_CONTROL_PAYLOAD` (125) bytes and is never
  fragmented. This is the bound that makes echoing a Ping's payload back from a
  fixed buffer safe, and it is enforced at parse time rather than asserted at send
  time — the size is the peer's, so an assertion on it hands the peer the process.
  `SendPing`/`SendPong` return false on an oversized payload instead of asserting.
- The high bit of a 64-bit length must be zero. A frame payload, and a fragmented
  message's running total, are bounded by the per-connection maximum
  (`DEFAULT_MAX_MESSAGE_SIZE`, lowerable via `SetMaxMessageSize`). Exceeding it is
  close `1009`.
- A Continuation frame requires an open message; a new data frame while one is open
  is refused. Both are close `1002`. `ConnectionBase::ProtocolError()` reports the
  code that was sent.
- The handshake copies a header name out of the receive buffer before reading its
  value, because reading the value refills the buffer and takes the name's
  terminator with it.

## Rationale

Failing the connection rather than salvaging the request is the whole point.
Salvage requires deciding what the peer meant, and every such decision is a place
where this parser and some other reader of the same bytes can differ. There is no
partial interpretation of a badly framed message that is safe to hand a handler —
not the body prefix that did arrive, and not the bytes behind it.

The costs are all on the error path. The well-formed request pays one predicate per
chunk-size digit, one comparison for the byte after a CR, and two counters over the
header block. The parse loop, the zero-copy chunk delivery, and the buffer mechanics
are unchanged.

## Covenants

Negative-first — what this design forbids:

- **No field is decoded as a wider grammar than it claims.** A hex field accepts
  hex digits; a decimal field accepts decimal digits. Masking a byte into range, or
  skipping what does not fit, is forbidden — those are how a field comes to name a
  length the sender never wrote.
- **A delimiter that has not fully arrived is not a delimiter.** A lone CR at the
  end of the buffer is not a line ending. A parser must wait for the rest or fail;
  it must never advance as though the missing byte were what it expected.
- **No delimiter is stepped over unread.** Every CRLF the framing depends on — a
  header line's, a chunk's, a trailer's — is compared, not assumed.
- **No pointer into the receive buffer outlives a refill.** A chunk handed to a
  caller is valid until that caller asks for more; the parser must not refill
  before returning it, and no length taken from the buffer (a `strlen` over a
  name, a view kept across a phase) may be measured after the bytes moved.
- **A message with two framings has none.** Content-Length with Transfer-Encoding,
  or two disagreeing Content-Lengths, is refused. Preferring one over the other is
  forbidden: the preference is exactly what a second reader may not share.
- **A failed request never becomes the next request.** Once framing fails, the
  buffered bytes are discarded rather than re-parsed, and keep-alive is off. This
  is the invariant the rest of the rules exist to protect.
- **A peer-supplied size never terminates the process.** Control-frame and payload
  bounds are enforced by refusing, never by `assert`. An assertion on an
  attacker-chosen value is a remote abort.
- **Bounds are not advisory.** The header count, header byte, chunk size, and
  message size ceilings are enforced in the parser, not left to a handler to
  re-derive. A handler that never reads the headers still gets the bound.

## Residual

- **Connection count and connection lifetime are not bounded here.** A peer that
  opens many connections, or holds one open sending a well-formed byte every few
  seconds, is bounded only by the recv timeout. That belongs to the server and its
  admission policy, not to the parser.
- **Non-minimal WebSocket length encodings are accepted.** A 16- or 64-bit length
  field holding a value that would fit in a narrower form is well-defined and
  bounded; it is not rejected.
- **Content-Length itself carries no ceiling.** Bodies stream in buffer-sized
  pieces and are bounded by the recv timeout, so the ceiling would only be a policy
  choice a consumer is better placed to make. The chunk-size ceiling exists because
  that value is arithmetic the parser performs, not just a total it reports.
