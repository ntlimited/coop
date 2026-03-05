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

// ConnectionBase is the handler-facing interface. Handlers take ConnectionBase& for transport-
// agnostic request processing. Virtual dispatch at the handler boundary; the parser internals
// (ConnectionImpl) use CRTP for zero-overhead buffer access.
//
struct ConnectionBase
{
    static constexpr size_t DEFAULT_BUFFER_SIZE = 2048;

    virtual ~ConnectionBase() = default;

    // Phase 1: Request line. Lazy, memoized. Null = parse failure.
    //
    virtual RequestLine* GetRequestLine() = 0;

    // Phase 2: GET args (query string). Sequential: NextArgName() then ReadArgValue() or
    // SkipArgValue(). Null name = no more args.
    //
    virtual const char* NextArgName() = 0;
    virtual Chunk* ReadArgValue() = 0;
    virtual void SkipArgValue() = 0;
    virtual void SkipArgs() = 0;

    // Phase 3: Headers. Same pattern as args. Implicitly skips remaining args.
    // Null name = no more headers.
    //
    virtual const char* NextHeaderName() = 0;
    virtual Chunk* ReadHeaderValue() = 0;
    virtual void SkipHeaderValue() = 0;
    virtual void SkipHeaders() = 0;

    // Phase 4: Body. Handles Transfer-Encoding: chunked internally. Null = end/error.
    //
    virtual Chunk* ReadBody() = 0;
    virtual void SkipBody() = 0;
    virtual int64_t ContentLength() = 0;

    // Response methods. Return false on send failure. Callers must not call send methods after
    // a failure (asserts in debug). Use SendError() to check.
    //
    virtual bool Send(int status, const char* contentType, const void* body, size_t size) = 0;
    virtual bool Send(int status, const char* contentType, const std::string& body) = 0;
    virtual bool SendHeaders(int status, const char* contentType, size_t contentLength) = 0;
    virtual bool BeginChunked(int status, const char* contentType) = 0;
    virtual bool SendChunk(const void* data, size_t size) = 0;
    virtual bool EndChunked() = 0;
    virtual bool EndChunked(const void* lastChunkData, size_t lastChunkSize) = 0;
    virtual bool Sendfile(int fileFd, off_t offset, size_t count) = 0;

    virtual bool SendError() const = 0;
    virtual void Reset() = 0;
    virtual bool KeepAlive() const = 0;
    virtual io::Descriptor& GetDescriptor() = 0;
    virtual Cooperator* GetCooperator() = 0;
};

// ConnectionImpl<Derived> is the CRTP parser implementation. All parser state lives here; buffer
// and transport access go through the Derived type. This gives the parser zero-overhead buffer
// access (compile-time offset from `this`) while keeping the implementation in connection.cpp
// via explicit template instantiation.
//
template<typename Derived>
struct ConnectionImpl : ConnectionBase
{
    ConnectionImpl(io::Descriptor& desc, Context* ctx, Cooperator* co,
                   time::Interval timeout);

    // ConnectionBase overrides — implemented in connection.cpp
    //
    RequestLine* GetRequestLine() override;
    const char* NextArgName() override;
    Chunk* ReadArgValue() override;
    void SkipArgValue() override;
    void SkipArgs() override;
    const char* NextHeaderName() override;
    Chunk* ReadHeaderValue() override;
    void SkipHeaderValue() override;
    void SkipHeaders() override;
    Chunk* ReadBody() override;
    void SkipBody() override;
    int64_t ContentLength() override;
    bool Send(int status, const char* contentType, const void* body, size_t size) override;
    bool Send(int status, const char* contentType, const std::string& body) override;
    bool SendHeaders(int status, const char* contentType, size_t contentLength) override;
    bool BeginChunked(int status, const char* contentType) override;
    bool SendChunk(const void* data, size_t size) override;
    bool EndChunked() override;
    bool EndChunked(const void* lastChunkData, size_t lastChunkSize) override;
    bool Sendfile(int fileFd, off_t offset, size_t count) override;
    bool SendError() const override { return m_sendError; }
    void Reset() override;
    bool KeepAlive() const override { return m_keepAlive && !m_clientClose; }
    io::Descriptor& GetDescriptor() override { return m_desc; }
    Cooperator* GetCooperator() override { return m_co; }

  private:
    // Buffer access via CRTP — resolved to compile-time offset, no pointer indirection
    //
    char* Buf() { return static_cast<Derived*>(this)->m_buf; }
    size_t BufSize() const { return static_cast<const Derived*>(this)->m_bufSize; }

    // Transport dispatch via CRTP — fully inlined, no virtual call
    //
    int TransportRecv(void* buf, size_t size, int flags, time::Interval timeout)
    {
        return static_cast<Derived*>(this)->DoRecv(buf, size, flags, timeout);
    }

    int TransportSendAll(const void* buf, size_t size)
    {
        return static_cast<Derived*>(this)->DoSendAll(buf, size);
    }

    int TransportWritevAll(struct iovec* iov, int iovcnt)
    {
        return static_cast<Derived*>(this)->DoWritevAll(iov, iovcnt);
    }

    int TransportSendfileAll(int in_fd, off_t offset, size_t count)
    {
        return static_cast<Derived*>(this)->DoSendfileAll(in_fd, offset, count);
    }

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
    void SkipToHeaders();
    static const char* StatusText(int code);
    const char* ConnectionHeaderValue() const;
    bool SendRaw(const void* data, size_t size);
    bool SendWritev(struct iovec* iov, int iovcnt);

    io::Descriptor& m_desc;
    Context*        m_ctx;
    Cooperator*     m_co;
    time::Interval  m_timeout;

    size_t          m_bufLen;
    size_t          m_parsePos;

    Phase           m_phase;
    int64_t         m_contentLength;
    bool            m_chunkedBody;
    size_t          m_bodyRemaining;

    RequestLine     m_requestLine;
    Chunk           m_chunk;
    bool            m_requestLineParsed;
    bool            m_chunkedDone;
    bool            m_valueConsumed;

    bool            m_pendingContentLength;
    bool            m_pendingTransferEncoding;
    bool            m_pendingConnection;

    bool            m_chunkedHeadersPending;
    int             m_chunkedStatus;
    const char*     m_chunkedContentType;

    bool            m_keepAlive;
    bool            m_clientClose;
    bool            m_sendError;
};

// Connection<Transport> is the final, concrete HTTP connection. The transport template parameter
// controls I/O dispatch (PlaintextTransport, TlsTransport, etc.) — fully inlined via CRTP, no
// virtual dispatch for I/O in the parser hot path.
//
// The recv buffer is a trailing flexible array (`m_buf[0]`). Allocate via
// `coop::Alloc<Connection<T>>(ctx, bufferSize, ...)` from the context's bump heap. Buffer access
// in the parser is a direct offset from `this` — zero pointer indirection.
//
template<typename Transport>
struct Connection final : ConnectionImpl<Connection<Transport>>
{
    Connection(Transport transport, Context* ctx, Cooperator* co,
               size_t bufSize,
               time::Interval timeout = std::chrono::seconds(30))
    : ConnectionImpl<Connection<Transport>>(transport.Descriptor(), ctx, co, timeout)
    , m_transport(transport)
    , m_bufSize(bufSize)
    {}

    // CRTP transport dispatch — called by ConnectionImpl, fully inlined
    //
    int DoRecv(void* buf, size_t size, int flags, time::Interval timeout)
    {
        return m_transport.Recv(buf, size, flags, timeout);
    }

    int DoSendAll(const void* buf, size_t size)
    {
        return m_transport.SendAll(buf, size);
    }

    int DoWritevAll(struct iovec* iov, int iovcnt)
    {
        return m_transport.WritevAll(iov, iovcnt);
    }

    int DoSendfileAll(int in_fd, off_t offset, size_t count)
    {
        return m_transport.SendfileAll(in_fd, offset, count);
    }

    Transport       m_transport;
    size_t          m_bufSize;
    char            m_buf[0];   // trailing — MUST BE LAST
};

} // end namespace coop::http
} // end namespace coop
