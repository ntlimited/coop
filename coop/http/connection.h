#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/uio.h>

#include "coop/io/descriptor.h"
#include "coop/time/interval.h"

namespace coop
{

struct Context;
struct Cooperator;

namespace http
{

// Bounded element from the request line. String_views point into the recv buffer — valid until
// the next operation that may compact the buffer (advancing phases, reading more data).
//
struct RequestLine
{
    std::string_view method;
    std::string_view path;      // before '?'
};

// Zero-copy chunk from the recv buffer. Returned by read methods for unbounded elements (arg
// values, header values, body). Null pointer return = failure/end.
//
struct Chunk
{
    const void* data;
    size_t size;
    bool complete;              // true if this is the last chunk of the current element
};

// ConnectionBase is the transport-agnostic HTTP connection parser and response writer. Owns a 2KB
// sliding-window recv buffer. Parsing is strictly sequential (request line -> args -> headers ->
// body), lazy, and memoized. Each phase implicitly skips the previous phase if not consumed.
//
// Handlers that don't need transport-specific access should take ConnectionBase& — this is the
// "sugar" path that works identically for plaintext and TLS connections.
//
struct ConnectionBase
{
    static constexpr size_t BUFFER_SIZE = 2048;

    ConnectionBase(io::Descriptor& desc, Context* ctx, Cooperator* co,
                   time::Interval timeout = std::chrono::seconds(30));

    virtual ~ConnectionBase() = default;

    // Phase 1: Request line. Lazy, memoized. Null = parse failure.
    //
    RequestLine* GetRequestLine();

    // Phase 2: GET args (query string). Sequential: NextArgName() then ReadArgValue() or
    // SkipArgValue(). Null name = no more args.
    //
    const char* NextArgName();
    Chunk* ReadArgValue();
    void SkipArgValue();
    void SkipArgs();

    // Phase 3: Headers. Same pattern as args. Implicitly skips remaining args.
    // Null name = no more headers.
    //
    const char* NextHeaderName();
    Chunk* ReadHeaderValue();
    void SkipHeaderValue();
    void SkipHeaders();

    // Phase 4: Body. Handles Transfer-Encoding: chunked internally. Null = end/error.
    //
    Chunk* ReadBody();
    void SkipBody();
    int64_t ContentLength();

    // Response methods. Return false on send failure. Callers must not call send methods after
    // a failure (asserts in debug). Use SendError() to check.
    //
    bool Send(int status, const char* contentType, const void* body, size_t size);
    bool Send(int status, const char* contentType, const std::string& body);
    bool SendHeaders(int status, const char* contentType, size_t contentLength);
    bool BeginChunked(int status, const char* contentType);
    bool SendChunk(const void* data, size_t size);
    bool EndChunked();
    bool EndChunked(const void* lastChunkData, size_t lastChunkSize);
    bool Sendfile(int fileFd, off_t offset, size_t count);

    bool SendError() const { return m_sendError; }

    // Keep-alive support. Reset() reinitializes parser state for the next request, preserving
    // leftover buffer data from pipelining. Call SkipBody() before Reset() if the handler may
    // not have consumed the request body.
    //
    void Reset();
    bool KeepAlive() const { return m_keepAlive && !m_clientClose; }

    // Accessors
    //
    io::Descriptor& GetDescriptor() { return m_desc; }
    Cooperator* GetCooperator() { return m_co; }

  protected:
    // Transport I/O dispatch — overridden by Connection<Transport> with final methods.
    //
    virtual int TransportRecv(void* buf, size_t size, int flags, time::Interval timeout) = 0;
    virtual int TransportSendAll(const void* buf, size_t size) = 0;
    virtual int TransportWritevAll(struct iovec* iov, int iovcnt) = 0;
    virtual int TransportSendfileAll(int in_fd, off_t offset, size_t count) = 0;

  private:
    enum Phase
    {
        REQUEST_LINE,
        ARGS,
        HEADERS,
        BODY,
        DONE,
    };

    // Buffer management
    //
    int RecvMore();
    void Compact();

    // Internal parsing helpers
    //
    bool ParseRequestLine();
    bool AdvanceToPhase(Phase target);
    Chunk* ReadChunkedBody();

    // Advance parsePos past the rest of the request line (\r\n) into headers territory
    //
    void SkipToHeaders();

    // Status text for HTTP response codes
    //
    static const char* StatusText(int code);

    // Returns "keep-alive" or "close" for the Connection response header
    //
    const char* ConnectionHeaderValue() const;

    // Send raw bytes via transport. Returns false on failure and poisons the connection.
    //
    bool SendRaw(const void* data, size_t size);

    // Scatter-gather send via transport. Returns false on failure and poisons.
    //
    bool SendWritev(struct iovec* iov, int iovcnt);

  protected:
    io::Descriptor& m_desc;
    Context*        m_ctx;
    Cooperator*     m_co;
    time::Interval  m_timeout;

  private:
    char            m_buf[BUFFER_SIZE];
    size_t          m_bufLen;
    size_t          m_parsePos;

    Phase           m_phase;
    int64_t         m_contentLength;
    bool            m_chunkedBody;
    size_t          m_bodyRemaining;    // bytes remaining for Content-Length or current chunk

    // Flyweight members returned as pointers
    //
    RequestLine     m_requestLine;
    Chunk           m_chunk;
    bool            m_requestLineParsed;

    // State for chunked body parsing
    //
    bool            m_chunkedDone;

    // State for value reading (args/headers)
    //
    bool            m_valueConsumed;

    // Flags for detecting Content-Length / Transfer-Encoding / Connection during header iteration
    //
    bool            m_pendingContentLength;
    bool            m_pendingTransferEncoding;
    bool            m_pendingConnection;

    // Deferred chunked headers — BeginChunked saves params, first SendChunk sends them together
    //
    bool            m_chunkedHeadersPending;
    int             m_chunkedStatus;
    const char*     m_chunkedContentType;

    // Keep-alive state
    //
    bool            m_keepAlive;
    bool            m_clientClose;

    // Sticky error flag — once a send fails, all subsequent sends are no-ops
    //
    bool            m_sendError;
};

// Connection<Transport> is the concrete, statically-typed HTTP connection. The transport template
// parameter controls I/O dispatch (PlaintextTransport, TlsTransport, etc.). Marked final so the
// compiler can devirtualize the transport calls when the concrete type is visible.
//
// For zero-overhead I/O dispatch, use Connection<T>& directly. For transport-agnostic handlers,
// use ConnectionBase& (the sugar path — virtual dispatch, ~2ns per I/O call).
//
template <typename Transport>
struct Connection final : ConnectionBase
{
    Connection(Transport transport, Context* ctx, Cooperator* co,
               time::Interval timeout = std::chrono::seconds(30))
    : ConnectionBase(transport.Descriptor(), ctx, co, timeout)
    , m_transport(transport)
    {}

  protected:
    int TransportRecv(void* buf, size_t size, int flags, time::Interval timeout) override final
    {
        return m_transport.Recv(buf, size, flags, timeout);
    }

    int TransportSendAll(const void* buf, size_t size) override final
    {
        return m_transport.SendAll(buf, size);
    }

    int TransportWritevAll(struct iovec* iov, int iovcnt) override final
    {
        return m_transport.WritevAll(iov, iovcnt);
    }

    int TransportSendfileAll(int in_fd, off_t offset, size_t count) override final
    {
        return m_transport.SendfileAll(in_fd, offset, count);
    }

  private:
    Transport m_transport;
};

} // end namespace coop::http
} // end namespace coop
