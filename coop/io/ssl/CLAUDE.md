# coop/io/ssl/ — SSL/TLS Internals

Two BIO modes for TLS connections:

## Memory BIO (default)

OpenSSL uses memory BIOs decoupled from the socket. All I/O goes through staging buffers:
`SSL_write -> wbio -> FlushWrite -> io::Send`. Works with any socket type (TCP, AF_UNIX).
Requires a caller-provided staging buffer (16KB recommended).
```cpp
ssl::Connection conn(sslCtx, desc, buffer, sizeof(buffer));
```

## Socket BIO (`ssl::SocketBio` tag)

OpenSSL operates on the real socket fd via `SSL_set_fd`. The handshake uses `io::Poll` for
cooperative waiting on WANT_READ/WANT_WRITE. After handshake, if kTLS activates,
`ssl::Send`/`ssl::Recv` bypass OpenSSL entirely — `io::Send`/`io::Recv` go straight to the
kernel which handles crypto transparently. No staging buffer needed.
```cpp
sslCtx.EnableKTLS();  // must be called before creating connections
ssl::Connection conn(sslCtx, desc, ssl::SocketBio{});
conn.Handshake();     // probes kTLS activation (sets m_ktlsTx, m_ktlsRx)
```

## kTLS Activation Requirements

TCP socket (not AF_UNIX), `EnableKTLS()` on the ssl::Context, kernel `tls` module loaded, and a
cipher suite the kernel supports. TLS 1.3 typically gets TX only; TLS 1.2 can get both TX+RX.
Falls back gracefully — socket BIO without kTLS still works, just uses `SSL_write`/`SSL_read` +
`io::Poll` instead of the memory BIO staging path.

## TCP_NODELAY

Set automatically in the socket BIO constructor. This is critical: kTLS frames each `write()` as
a complete TLS record, and Nagle's algorithm delays sending small TCP segments. The interaction is
catastrophic — without `TCP_NODELAY`, kTLS is 500x slower than plaintext on loopback at 16KB
messages. With `TCP_NODELAY`, kTLS performs within ~10% of memory BIO at 16KB (187us vs 169us
in benchmarks).

## Data Path Dispatch

In `ssl::Send` and `ssl::Recv`:
1. `m_ktlsTx`/`m_ktlsRx` true -> `write()`/`read()` directly + `io::Poll` on EAGAIN
   (zero OpenSSL involvement; synchronous when socket buffer available)
2. `m_buffer == nullptr` (socket BIO, no kTLS) -> `SSL_write`/`SSL_read` + `io::Poll`
3. `m_buffer != nullptr` (memory BIO) -> existing `FlushWrite`/`FeedRead` path

Both kTLS and socket BIO paths use direct syscalls with `io::Poll` fallback, avoiding uring
for the common case where the syscall succeeds immediately. This is critical for throughput —
routing every send through `io::Send` (uring SQE/CQE) adds ~20us per-op overhead that
dominates the crypto savings kTLS provides.
