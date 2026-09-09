#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <sys/types.h>

#include "detail/parser_buffer.hpp"
#include "types.h"
#include "coop/io/descriptor.h"
#include "coop/time/interval.h"

namespace coop
{
namespace http
{

// A checked body pull borrows a Chunk from its connection. Truthiness means data;
// Complete() means a successful terminal pull. Error() is a negative errno on failure.
// Like Chunk itself, both the descriptor and its bytes expire on the next parser call.
//
struct BodyResult
{
    Chunk* chunk;
    int error;
    explicit operator bool() const { return chunk != nullptr; }
    Chunk* operator->() const { return chunk; }
    Chunk& operator*() const { return *chunk; }
    bool Complete() const { return !chunk && error == 0; }
    int Error() const { return error; }
};

// ClientConnectionImpl<Derived> is the CRTP response parser and request sender. Mirrors the
// server-side ConnectionImpl pattern: buffer management, header/body parsing via sequential
// phases, CRTP for zero-overhead transport dispatch.
//
// Does not take a Context or Cooperator — the caller must be running on a coop context
// for native transports. One request is outstanding at a time; Reset after a reusable
// final response before beginning another. No scheduling, pooling, retries or drains
// happen implicitly at reset/destruction.
//
// Phases: RESPONSE_LINE -> HEADERS -> BODY -> DONE (no ARGS phase — responses don't have
// query strings).
//
template<typename Derived>
struct ClientConnectionImpl : detail::ParserBuffer<Derived>
{
    friend struct detail::ParserBuffer<Derived>;

    ClientConnectionImpl(const char* host, time::Interval timeout);

    // --- Request sending ---
    //
    // Send an HTTP request. Returns false on send failure.
    //
    bool SendRequest(const char* method, const char* path,
                     const char* contentType = nullptr,
                     const void* body = nullptr, size_t bodySize = 0);

    bool Get(const char* path);
    bool Post(const char* path, const char* contentType,
              const void* body, size_t bodySize);
    bool Head(const char* path);

    // Component request API — for requests needing arbitrary headers (auth signatures,
    // ranges, conditionals) or streamed bodies. BeginRequest writes the request line and
    // Host header; AppendHeader appends one header; EndHeaders closes the block and
    // flushes. The caller owns body framing: append a Content-Length (or Transfer-
    // Encoding) header, then stream the body with SendBody. SendBody writes verbatim:
    // specifying chunked does not generate size lines or terminators for the caller.
    // Methods and header values are caller-authored wire syntax, not escaped strings.
    //
    bool BeginRequest(const char* method, const char* path);
    bool AppendHeader(const char* name, std::string_view value);
    bool AppendHeader(const char* name, size_t value);
    bool EndHeaders();
    bool SendBody(const void* data, size_t size);

    // Stream a request body straight from a file — the send-from-disk idiom (a PUT of a
    // cached object). Flushes any buffered headers, then sendfile through the transport
    // (kTLS-aware on TLS). The caller appended the matching Content-Length header.
    // Large counts are split into representable transport operations; offset overflow
    // is rejected before headers or file bytes are sent.
    //
    bool SendBodyFromFile(int fileFd, off_t offset, size_t count);

    // --- Response parsing ---
    //
    // Phase 1: Status line. Null on parse failure, connection closed, or timeout.
    // ResponseLine::reason views recv-buffer bytes that are discarded once later
    // parsing compacts the buffer — copy it (or forward it, e.g. into BeginResponse)
    // before iterating headers. Repeat calls after invalidation return null (and
    // assert in debug).
    //
    ResponseLine* GetResponseLine();

    // Phase 2: Headers. Names are NUL-terminated borrowed spans and must fit the
    // caller's receive buffer; values stream across arbitrarily many receives. Each
    // returned name/value remains valid until the next parsing/reset operation.
    // Null name = end of header block or failure (Error() distinguishes).
    //
    const char* NextHeaderName();
    Chunk* ReadHeaderValue();
    void SkipHeaderValue();
    void SkipHeaders();

    // Phase 3: Body. Content-Length, chunked (including trailers), and close-delimited
    // bodies are supported. Other transfer/content codings remain opaque: no decoder
    // is installed implicitly. HEAD, 1xx, 204, 304 and successful CONNECT responses
    // have no HTTP body; advertised Content-Length remains metadata except for successful
    // CONNECT, whose Content-Length/Transfer-Encoding fields are ignored.
    // ReadBody's legacy null conflates completion/failure; prefer checked NextBody.
    // Chunk::complete means the end of the current wire chunk for chunked bodies, or
    // the end of a fixed-length body. Complete() is the whole-message result, including
    // chunk terminators and trailers. Close-delimited completion needs an EOF pull.
    //
    Chunk* ReadBody();

    // Checked pull: borrowed data, successful end, and failure are distinct. The view
    // remains valid until the next parsing/reset operation. No body bytes are owned.
    //
    BodyResult NextBody();
    bool SkipBody();

    // Completes header parsing and returns the advertised length, or -1 if absent.
    // Does not turn an unknown length into an empty body. Inspect Error() on failure.
    //
    int64_t ContentLength();

    // Receive the remaining response body directly into a file at `offset` — the
    // cache-fill idiom (GET from origin, body to disk). Buffered bytes are written first;
    // the framed remainder splices socket -> pipe -> page cache on plaintext transports,
    // and bounces through the parser buffer for TLS, chunked, or close-delimited bodies.
    // Framing-only responses return 0. Returns total bytes written or negative errno;
    // an error may follow partial file writes. File/socket failures are terminal.
    //
    int64_t ReadBodyToFile(int fileFd, off_t offset);

    // KeepAlive is the peer's persistence policy, not proof of message completion.
    // Reusable additionally requires a complete final response and no failure.
    // Error is zero until failure, otherwise a negative errno: -EPROTO malformed
    // framing, -ECONNRESET truncated response, -EMSGSIZE a contiguous token exceeds
    // the caller's recv buffer, or the unchanged transport error.
    //
    bool Complete() const { return m_phase == DONE && m_error == 0; }
    int Error() const { return m_error; }
    bool KeepAlive() const { return m_keepAlive && !m_serverClose && !m_error; }
    bool Reusable() const
    {
        return Complete() && KeepAlive() && m_responseLine.status >= 200 && !m_upgraded;
    }

    // Reset only after a reusable final response; never drains or discards an error.
    // AdvanceResponse only after an informational response (except 101), preserving
    // the request method for the subsequent response. Both invalidate borrowed views.
    //
    bool Reset();
    bool AdvanceResponse();

  private:
    char* RecvBuf() { return static_cast<Derived*>(this)->m_buf; }
    size_t RecvBufSize() const { return static_cast<const Derived*>(this)->m_recvBufSize; }
    char* SendBuf()
    {
        return static_cast<Derived*>(this)->m_buf +
               static_cast<const Derived*>(this)->m_recvBufSize;
    }
    size_t SendBufSize() const { return static_cast<const Derived*>(this)->m_sendBufSize; }

    int TransportRecv(void* buf, size_t size, int flags, time::Interval timeout)
    {
        return static_cast<Derived*>(this)->DoRecv(buf, size, flags, timeout);
    }

    int TransportSendAll(const void* buf, size_t size)
    {
        return static_cast<Derived*>(this)->DoSendAll(buf, size);
    }

    int TransportSendfileAll(int in_fd, off_t offset, size_t count)
    {
        return static_cast<Derived*>(this)->DoSendfileAll(in_fd, offset, count);
    }

    // Write buffer management
    //
    bool Append(const void* data, size_t size);
    bool Flush();
    bool AppendUInt(size_t val);
    template<size_t N>
    bool AppendLiteral(const char (&s)[N]);

    enum Phase
    {
        RESPONSE_LINE,
        HEADERS,
        BODY,
        DONE,
    };

    using detail::ParserBuffer<Derived>::m_bufLen;
    using detail::ParserBuffer<Derived>::m_parsePos;
    using detail::ParserBuffer<Derived>::m_bufEpoch;
    using detail::ParserBuffer<Derived>::RecvMore;
    using detail::ParserBuffer<Derived>::Compact;
    using detail::ParserBuffer<Derived>::Win;

    bool RecvAborted() const { return false; }   // no context; kill-awareness is the
                                                 // caller's (guard/timeout) concern

    bool Fail(int error);
    bool Receive(bool eofCompletes = false);
    bool Ensure(size_t size);
    bool ConsumeCrlf();
    void ClearResponse();
    bool FinishHeaders();
    bool FeedHeaderValue(const char* data, size_t size, bool final);
    bool FinishHeaderToken();
    bool ParseResponseLine();
    bool AdvanceToPhase(Phase target);
    Chunk* ReadChunkedBody();
    bool SendRaw(const void* data, size_t size);

    const char*     m_host;
    time::Interval  m_timeout;

    size_t          m_sendLen;

    Phase           m_phase;
    int64_t         m_contentLength;
    bool            m_chunkedBody;
    size_t          m_bodyRemaining;

    ResponseLine    m_responseLine;
    Chunk           m_chunk;
    uint32_t        m_responseLineEpoch; // m_bufEpoch when the response line was parsed
    bool            m_responseLineParsed;
    int             m_error;
    bool            m_needChunkCrlf;
    bool            m_closeDelimited;
    bool            m_transferEncoding; // at least one parsed transfer coding
    bool            m_hasTransferEncoding; // field presence, including empty lists
    bool            m_upgraded;
    bool            m_isConnect;
    bool            m_valueStarted;
    uint64_t        m_fieldNumber;
    uint8_t         m_fieldState;
    uint8_t         m_tokenLength;
    uint8_t         m_tokenMatches;
    bool            m_lastTokenChunked;
    bool            m_valueConsumed;

    bool            m_pendingContentLength;
    bool            m_pendingTransferEncoding;
    bool            m_pendingConnection;

    bool            m_keepAlive;
    bool            m_serverClose;
    bool            m_isHead;
};

// ClientConnection<Transport> is the final concrete type. Same trailing-buffer pattern as the
// server-side Connection: [object] [recv buf ... recvBufSize] [send buf ... sendBufSize].
// Allocate via ctx->Allocate<ClientConnection<T>>(ExtraBytes(...), ...). The hostname
// and transport dependencies must outlive the connection. Receive size >= 2, send size
// > 0; status lines and header names must fit the chosen receive buffer. Header values,
// body chunks, chunk extensions and trailers are streamed without a fixed size limit.
//
template<typename Transport>
struct ClientConnection final : ClientConnectionImpl<ClientConnection<Transport>>
{
    static constexpr bool kSpliceable = Transport::kSpliceable;

    static constexpr size_t DEFAULT_RECV_SIZE = 4096;
    static constexpr size_t DEFAULT_SEND_SIZE = 512;

    static constexpr size_t ExtraBytes(
        size_t recvBufSize = DEFAULT_RECV_SIZE,
        size_t sendBufSize = DEFAULT_SEND_SIZE)
    {
        return recvBufSize + sendBufSize;
    }

    ClientConnection(Transport transport, const char* host,
                     size_t recvBufSize = DEFAULT_RECV_SIZE,
                     size_t sendBufSize = DEFAULT_SEND_SIZE,
                     time::Interval timeout = std::chrono::seconds(30))
    : ClientConnectionImpl<ClientConnection<Transport>>(
          host, timeout)
    , m_transport(transport)
    , m_recvBufSize(recvBufSize)
    , m_sendBufSize(sendBufSize)
    {
        assert(host && recvBufSize >= 2 && sendBufSize > 0);
    }

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    int DoRecv(void* buf, size_t size, int flags, time::Interval timeout)
    {
        return m_transport.Recv(buf, size, flags, timeout);
    }

    int DoSendAll(const void* buf, size_t size)
    {
        return m_transport.SendAll(buf, size);
    }

    int DoSendfileAll(int in_fd, off_t offset, size_t count)
    {
        return m_transport.SendfileAll(in_fd, offset, count);
    }

    Transport       m_transport;
    size_t          m_recvBufSize;
    size_t          m_sendBufSize;
    char            m_buf[0];   // trailing: [recv ... recvBufSize] [send ... sendBufSize]
};

} // end namespace coop::http
} // end namespace coop
