#pragma once

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

// ClientConnectionImpl<Derived> is the CRTP response parser and request sender. Mirrors the
// server-side ConnectionImpl pattern: buffer management, header/body parsing via sequential
// phases, CRTP for zero-overhead transport dispatch.
//
// Does not take a Context or Cooperator — the caller must be running on a coop context.
//
// Phases: RESPONSE_LINE -> HEADERS -> BODY -> DONE (no ARGS phase — responses don't have
// query strings).
//
template<typename Derived>
struct ClientConnectionImpl : detail::ParserBuffer<Derived>
{
    friend struct detail::ParserBuffer<Derived>;

    ClientConnectionImpl(io::Descriptor& desc, const char* host,
                         time::Interval timeout);

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
    // Encoding) header, then stream the body with SendBody.
    //
    bool BeginRequest(const char* method, const char* path);
    bool AppendHeader(const char* name, std::string_view value);
    bool AppendHeader(const char* name, size_t value);
    bool EndHeaders();
    bool SendBody(const void* data, size_t size);

    // Stream a request body straight from a file — the send-from-disk idiom (a PUT of a
    // cached object). Flushes any buffered headers, then sendfile through the transport
    // (kTLS-aware on TLS). The caller appended the matching Content-Length header.
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

    // Phase 2: Headers. Null name = end of header block.
    //
    const char* NextHeaderName();
    Chunk* ReadHeaderValue();
    void SkipHeaderValue();
    void SkipHeaders();

    // Phase 3: Body. Handles Content-Length and Transfer-Encoding: chunked. HEAD
    // responses and 204/304 statuses are framing-only: body reads end immediately,
    // whatever Content-Length said (which ContentLength() still reports, for forwarding).
    // Null = end of body or recv failure.
    //
    Chunk* ReadBody();
    void SkipBody();
    int64_t ContentLength();

    // Receive the remaining response body directly into a file at `offset` — the
    // cache-fill idiom (GET from origin, body to disk). Buffered bytes are written first;
    // the framed remainder splices socket -> pipe -> page cache on plaintext transports,
    // and bounces through the parser buffer for TLS or chunked bodies. Framing-only
    // responses (HEAD/204/304) return 0. Returns total bytes written or negative errno.
    //
    int64_t ReadBodyToFile(int fileFd, off_t offset);

    bool KeepAlive() const { return m_keepAlive && !m_serverClose; }
    void Reset();

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

    bool RecvAborted() const { return false; }   // no context; kill-awareness is the
                                                 // caller's (guard/timeout) concern

    bool ParseResponseLine();
    bool AdvanceToPhase(Phase target);
    Chunk* ReadChunkedBody();
    bool SendRaw(const void* data, size_t size);

    io::Descriptor& m_desc;
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
    bool            m_chunkedDone;
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
// Allocate via ctx->Allocate<ClientConnection<T>>(ExtraBytes(...), ...).
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
          transport.Descriptor(), host, timeout)
    , m_transport(transport)
    , m_recvBufSize(recvBufSize)
    , m_sendBufSize(sendBufSize)
    {}

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
