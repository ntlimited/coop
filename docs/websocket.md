# WebSocket server frames

The WebSocket layer exposes borrowed frame payloads and explicit sends over the HTTP
transports. It does not assemble messages, answer Ping frames, select subprotocols, or
schedule a Close handshake for the handler. Callers decide when to receive, retain,
reply, and stop. The parser validates the frame envelope and fragmentation sequence;
applications remain responsible for text/reason UTF-8 and Close status-code semantics.

## Upgrade and receive ownership

Call `ws::Upgrade(http)` after inspecting the request line and before iterating HTTP
headers. It validates an HTTP/1.1 GET with Host, Upgrade/Connection tokens, version 13,
a single base64-encoded 16-byte key, and no pending HTTP body; then sends 101.
Successful upgrade disables HTTP persistence so the server loop cannot reinterpret
WebSocket traffic as another HTTP request; the transport remains available to the handler. The helper
streams header fragments, retaining only the 24-byte key and 2-byte version. Long
Connection and Upgrade token lists do not require an owning string or a larger buffer.
HTTP parser limits still apply according to the server's configured policy.

An invalid handshake normally receives 400 and marks HTTP persistence off.
`ws::Upgrade(http, false)` leaves that error response to the caller. HTTP parser errors
follow the HTTP parser's own response policy; the helper never appends a second response
after a parser or transport failure. The helper negotiates no extensions or subprotocols;
a handler needing negotiation can use the HTTP component response API directly.

After a successful upgrade, construct `ws::Connection<Transport>` with
`http.LeftoverData()` and `http.LeftoverSize()` before destroying or advancing the HTTP
connection. These already-received bytes are copied once into the WebSocket receive
buffer. Size that buffer to hold all leftovers; silently discarding overflow is not a
supported handoff. The underlying transport and descriptor must outlive both objects.

WebSocket connections use contiguous trailing receive/send buffers, allocated with
`Connection<T>::ExtraBytes(recvSize, sendSize)`. The receive buffer must hold at least
12 bytes (the longest extended header after its first two bytes); send size must be
positive and fit `int`. These are debug-asserted construction preconditions. Connection
objects are noncopyable. A custom static transport includes
`coop/ws/detail/connection_impl.hpp`; native plaintext/TLS instantiations are in the library.

## Pulling payloads

`NextFrame()` returns a `Frame*` whose payload borrows the receive buffer and is unmasked
in place. Both descriptor and bytes remain valid until the next `NextFrame()` or
`SkipPayload()` call. Sending a frame does not invalidate received payload bytes.

`Frame::complete` means that the current wire frame's entire payload has been delivered.
`Frame::fin` is its wire FIN bit. A data message ends only when **both** are true;
a final frame can still arrive in several receive spans. Continuations expose the
original Text/Binary opcode. Ping, Pong, and Close spans remain separate, including
when interleaved with a fragmented data message.

Control frames cannot be fragmented on the wire, but their payloads can cross receive
boundaries. If replying to a Ping with one Pong, explicitly retain its spans in your own
125-byte buffer until `complete`. The same applies when inspecting a Close reason.
There is no implicit control-payload accumulator in each connection.

`SkipPayload()` explicitly drains the remainder of the current wire frame. A Close
remains readable or skippable through its last span. Subsequent `NextFrame()` calls
return null without consuming a following frame.

When `NextFrame()` returns null:

- `PeerClosed()` confirms the complete Close payload was delivered or skipped.
- `Error()` preserves negative transport errors; EOF before a complete Close is
  `-ECONNRESET`, and invalid framing is `-EPROTO`.
- `ProtocolError()` gives the WebSocket close code for an envelope/framing failure.

By default protocol errors attempt a Close reply, preserving existing behavior.
`SetAutoCloseOnError(false)` records the terminal error without writing anything, so
an enclosing protocol handler chooses its own response and teardown. No automatic Pong,
Close echo, timeout loop, or background drain occurs.

## Limits and sending

`SetMaxMessageSize(bytes)` chooses the cumulative data-message budget, including all
continuation frames. The default is 16 MiB; callers may lower or raise it, or use
`SIZE_MAX` to accept protocol-representable sizes. This is an admission check, not an
allocation: large frames stream through the existing receive buffer. Control frames
have their separate protocol limit of 125 bytes and do not consume the data budget.

`SendText` and `SendBinary` send one complete message. For caller-controlled fragmentation:

```cpp
ws.SendFragment(ws::Opcode::Binary, false, first.data(), first.size());
ws.SendPing(nullptr, 0); // control frames may interleave
ws.SendFragment(ws::Opcode::Continuation, true, last.data(), last.size());
```

Check every return value. Each successful call flushes that frame synchronously and
retains no payload pointers. A second data-message start while a fragmented message is
open, or a stray continuation, returns false before sending. Large direct payload sends
are split only at the transport's representable operation size; no extra body copy is
introduced. A write failure is sticky, because retrying a partially written frame would
corrupt the wire stream. `Close()` remains false after a failed Close send, and no new
data or Ping/Pong frames are sent after Close.

The transport's existing receive timeout behavior applies, including TLS's existing
timeout limitations. This API does not introduce an operation-wide deadline policy.
