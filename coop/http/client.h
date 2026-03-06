#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

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
struct ClientConnectionImpl
{
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

    // --- Response parsing ---
    //
    // Phase 1: Status line. Null on parse failure, connection closed, or timeout.
    //
    ResponseLine* GetResponseLine();

    // Phase 2: Headers. Null name = end of header block.
    //
    const char* NextHeaderName();
    Chunk* ReadHeaderValue();
    void SkipHeaderValue();
    void SkipHeaders();

    // Phase 3: Body. Handles Content-Length and Transfer-Encoding: chunked.
    // Null = end of body or recv failure.
    //
    Chunk* ReadBody();
    void SkipBody();
    int64_t ContentLength();

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

    int RecvMore();
    void Compact();

    bool ParseResponseLine();
    bool AdvanceToPhase(Phase target);
    Chunk* ReadChunkedBody();
    bool SendRaw(const void* data, size_t size);

    io::Descriptor& m_desc;
    const char*     m_host;
    time::Interval  m_timeout;

    size_t          m_bufLen;
    size_t          m_parsePos;
    size_t          m_sendLen;

    Phase           m_phase;
    int64_t         m_contentLength;
    bool            m_chunkedBody;
    size_t          m_bodyRemaining;

    ResponseLine    m_responseLine;
    Chunk           m_chunk;
    bool            m_responseLineParsed;
    bool            m_chunkedDone;
    bool            m_valueConsumed;

    bool            m_pendingContentLength;
    bool            m_pendingTransferEncoding;
    bool            m_pendingConnection;

    bool            m_keepAlive;
    bool            m_serverClose;
};

// ClientConnection<Transport> is the final concrete type. Same trailing-buffer pattern as the
// server-side Connection: [object] [recv buf ... recvBufSize] [send buf ... sendBufSize].
// Allocate via ctx->Allocate<ClientConnection<T>>(ExtraBytes(...), ...).
//
template<typename Transport>
struct ClientConnection final : ClientConnectionImpl<ClientConnection<Transport>>
{
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

    Transport       m_transport;
    size_t          m_recvBufSize;
    size_t          m_sendBufSize;
    char            m_buf[0];   // trailing: [recv ... recvBufSize] [send ... sendBufSize]
};

} // end namespace coop::http
} // end namespace coop
