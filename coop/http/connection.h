#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "detail/parser_buffer.hpp"
#include "types.h"
#include "coop/io/descriptor.h"
#include "coop/time/interval.h"

namespace coop
{

struct Context;
struct Cooperator;

namespace http
{

// Resource policy is separate from wire grammar. SIZE_MAX disables a ceiling; zero
// permits zero items/bytes. Options are copied once and persist across Reset().
//
enum class ParserErrorResponse { Automatic, Caller };
struct ServerParserOptions
{
    size_t maxHeaderCount = 100;
    size_t maxHeaderBytes = 8192;
    size_t maxChunkSize = size_t(1) << 30;
    ParserErrorResponse errorResponse = ParserErrorResponse::Automatic;
};

// ConnectionBase is the handler-facing interface. Handlers take ConnectionBase& for transport-
// agnostic request processing. Virtual dispatch at the handler boundary; the parser internals
// (ConnectionImpl) use CRTP for zero-overhead buffer access.
//
struct ConnectionBase
{
    static constexpr size_t DEFAULT_BUFFER_SIZE = 2048;
    static constexpr size_t DEFAULT_SEND_BUFFER_SIZE = 512;

    // Compatibility names for the default policy; callers may choose other limits.
    static constexpr size_t MAX_HEADER_COUNT = 100;
    static constexpr size_t MAX_HEADER_BYTES = 8192;
    static constexpr size_t MAX_CHUNK_SIZE = size_t(1) << 30;

    virtual ~ConnectionBase() = default;

    // Phase 1: Request line. Lazy, memoized. Null = parse failure — or, on a repeat
    // call, that the views have been invalidated by buffer movement (see below).
    //
    // View lifetime: RequestLine's string_views point into the recv buffer, and the
    // argument-name parsing tokenizes target/query in place; later parsing can discard the
    // bytes through compaction (split headers, body reads). Copy what must outlive argument or header
    // iteration — a proxy copies method/target before consuming arguments or headers. Repeat
    // GetRequestLine() calls after invalidation return null (and assert in debug)
    // rather than serving views into reused memory.
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

    // Phase 4: Body. NextBody returns borrowed data, successful completion, or errno.
    // ReadBody retains its legacy null=end/error interface; check Complete()/Error().
    // SkipBody explicitly drains. Reset refuses incomplete/failed/closing requests
    // without reading. Complete describes request consumption, not response delivery.
    //
    virtual Chunk* ReadBody() = 0;
    virtual BodyResult NextBody() = 0;
    virtual bool SkipBody() = 0;
    virtual bool Complete() const = 0;
    virtual int Error() const = 0;
    virtual bool Reusable() const = 0;
    virtual bool SetParserOptions(ServerParserOptions options) = 0;
    virtual const ServerParserOptions& GetParserOptions() const = 0;
    virtual int64_t ContentLength() = 0;

    // Receive the remaining body directly into a file at `offset` — the receive-to-disk
    // idiom. Body bytes already pulled into the recv buffer during header parsing are
    // written first; the framed remainder then moves socket -> pipe -> page cache without
    // visiting userspace on spliceable (plaintext) transports. TLS and chunked bodies
    // bounce through the parser buffer instead (correct, just copied). Consumes the body
    // to its end: the connection is positioned for Reset/keep-alive on success.
    //
    // Returns total bytes written, or negative errno (-ECONNRESET: peer closed mid-body).
    //
    virtual int64_t ReadBodyToFile(int fileFd, off_t offset) = 0;

    // Response methods return false on failure. Failures are sticky; later sends do
    // no IO. Caller-mode parser errors allow the handler to send its own response.
    //
    virtual bool Send(int status, const char* contentType, const void* body, size_t size) = 0;
    virtual bool Send(int status, const char* contentType, const std::string& body) = 0;
    virtual bool SendHeaders(int status, const char* contentType, size_t contentLength) = 0;
    virtual bool BeginChunked(int status, const char* contentType) = 0;
    virtual bool SendChunk(const void* data, size_t size) = 0;
    virtual bool EndChunked() = 0;
    virtual bool EndChunked(const void* lastChunkData, size_t lastChunkSize) = 0;
    virtual bool Sendfile(int fileFd, off_t offset, size_t count) = 0;

    // Component response API — for proxying and any response the composed methods above don't
    // cover. BeginResponse writes the status line: known codes use pre-compiled fragments, any
    // other code is formatted at runtime with `reason` (or a status-class default when empty).
    // AppendHeader appends one header line; EndHeaders closes the block with the framework's
    // Connection header and flushes. The caller owns body framing: append a Content-Length or
    // Transfer-Encoding header (or ForceClose), then stream body bytes via SendRawBytes — or
    // SendChunk/EndChunked when Transfer-Encoding: chunked was declared.
    //
    virtual bool BeginResponse(int status, std::string_view reason = {}) = 0;
    virtual bool AppendHeader(const char* name, std::string_view value) = 0;
    virtual bool AppendHeader(const char* name, size_t value) = 0;
    virtual bool EndHeaders() = 0;

    // Close the connection after the current response: the Connection header written by
    // EndHeaders (or the composed send methods) says close and the keep-alive loop exits.
    // For responses whose end can only be signaled by EOF, e.g. proxying an upstream
    // response that carries no length framing.
    //
    virtual void ForceClose() = 0;

    virtual bool SendError() const = 0;

    // HTTP status describing malformed framing or a policy limit (400, 413, 431).
    // Automatic mode answers once if no response began; Caller mode leaves response
    // generation to the handler. In both modes parsing is terminal and reuse is off.
    // Error() separately preserves negative errno, including transport/sink failures.
    //
    virtual int ParseError() const = 0;

    virtual bool Reset() = 0;
    virtual bool KeepAlive() const = 0;
    virtual io::Descriptor& GetDescriptor() = 0;
    virtual Cooperator* GetCooperator() = 0;

    // Protocol upgrade support. LeftoverData/Size return unconsumed bytes in the recv buffer
    // (data already recv'd past the HTTP headers). SendRawBytes flushes the send buffer and
    // sends arbitrary bytes through the transport — used for the 101 Switching Protocols
    // response which doesn't follow standard HTTP response formatting.
    //
    virtual const char* LeftoverData() = 0;
    virtual size_t LeftoverSize() = 0;
    virtual bool SendRawBytes(const void* data, size_t size) = 0;
};

// ConnectionImpl<Derived> is the CRTP parser implementation. All parser state lives here; buffer
// and transport access go through the Derived type. This gives the parser zero-overhead buffer
// access (compile-time offset from `this`) while keeping the implementation in connection.cpp
// via explicit native instantiation; include detail/server_impl.hpp for custom transports.
//
template<typename Derived>
struct ConnectionImpl : ConnectionBase, detail::ParserBuffer<Derived>
{
    friend struct detail::ParserBuffer<Derived>;

    ConnectionImpl(io::Descriptor& desc, Context* ctx, Cooperator* co,
                   time::Interval timeout, ServerParserOptions options);

    // ConnectionBase overrides — implemented in detail/server_impl.hpp
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
    BodyResult NextBody() override;
    bool SkipBody() override;
    bool Complete() const override { return m_phase == DONE && m_error == 0; }
    int Error() const override { return m_error; }
    bool Reusable() const override { return Complete() && KeepAlive() && !m_sendError; }
    const ServerParserOptions& GetParserOptions() const override { return m_options; }
    bool SetParserOptions(ServerParserOptions options) override
    {
        if (m_requestLineParsed || m_phase != REQUEST_LINE) return false;
        m_options = options;
        return true;
    }
    int64_t ContentLength() override;
    int64_t ReadBodyToFile(int fileFd, off_t offset) override;
    bool Send(int status, const char* contentType, const void* body, size_t size) override;
    bool Send(int status, const char* contentType, const std::string& body) override;
    bool SendHeaders(int status, const char* contentType, size_t contentLength) override;
    bool BeginChunked(int status, const char* contentType) override;
    bool SendChunk(const void* data, size_t size) override;
    bool EndChunked() override;
    bool EndChunked(const void* lastChunkData, size_t lastChunkSize) override;
    bool Sendfile(int fileFd, off_t offset, size_t count) override;
    bool BeginResponse(int status, std::string_view reason = {}) override;
    bool AppendHeader(const char* name, std::string_view value) override;
    bool AppendHeader(const char* name, size_t value) override;
    bool EndHeaders() override;
    void ForceClose() override { m_clientClose = true; }
    bool SendError() const override { return m_sendError; }
    int ParseError() const override { return m_parseError; }
    bool Reset() override;
    bool KeepAlive() const override { return m_keepAlive && !m_clientClose && !m_error; }
    io::Descriptor& GetDescriptor() override { return m_desc; }
    Cooperator* GetCooperator() override { return m_co; }

    const char* LeftoverData() override
    {
        return this->Win() + m_parsePos;
    }
    size_t LeftoverSize() override
    {
        return m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
    }
    bool SendRawBytes(const void* data, size_t size) override;

  private:
    // Buffer access via CRTP — resolved to compile-time offset, no pointer indirection
    //
    char* RecvBuf() { return static_cast<Derived*>(this)->m_buf; }
    size_t RecvBufSize() const { return static_cast<const Derived*>(this)->m_recvBufSize; }
    char* SendBuf() { return static_cast<Derived*>(this)->m_buf +
                             static_cast<const Derived*>(this)->m_recvBufSize; }
    size_t SendBufSize() const { return static_cast<const Derived*>(this)->m_sendBufSize; }

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

    int TransportSendfileAll(int in_fd, off_t offset, size_t count)
    {
        return static_cast<Derived*>(this)->DoSendfileAll(in_fd, offset, count);
    }

    // Write buffer management
    //
    bool Append(const void* data, size_t size);
    bool Flush();
    bool AppendUInt(size_t val);
    bool AppendHex(size_t val);
    template<size_t N>
    bool AppendLiteral(const char (&s)[N]);
    bool AppendConnectionTrailer();
    bool AppendStatusLine(int status, std::string_view reason);
    bool AppendChunkedHeaders();

    enum Phase
    {
        REQUEST_LINE,
        ARGS,
        HEADERS,
        BODY,
        DONE,
    };

    // Buffer mechanics live in detail::ParserBuffer (shared with the client parser);
    // using-declarations make the dependent-base names visible unqualified.
    //
    using detail::ParserBuffer<Derived>::m_bufLen;
    using detail::ParserBuffer<Derived>::m_parsePos;
    using detail::ParserBuffer<Derived>::m_bufEpoch;

    using detail::ParserBuffer<Derived>::Compact;
    using detail::ParserBuffer<Derived>::Win;

    bool RecvAborted() const;
    int RecvMore();
    bool Receive();
    bool Ensure(size_t size);
    bool ConsumeCrlf();
    bool Fail(int error);
    bool FailIo(int error);

    // Internal parsing helpers
    //
    bool ParseRequestLine();
    bool AdvanceToPhase(Phase target);
    Chunk* ReadChunkedBody();
    void SkipToHeaders();
    bool SendRaw(const void* data, size_t size);

    // Framing enforcement. FailRequest answers the peer once and makes the connection
    // terminal; every other helper returns false through it. All of them return false on
    // failure so a caller can `if (!X(...)) return nullptr;`.
    //
    bool FailRequest(int status);
    bool ChargeHeaderBytes(size_t bytes);
    bool FinishHeaders();
    bool FeedHeaderValue(const char* data, size_t size, bool final);
    bool FinishHeaderToken();

    // Automatic parser failures and IO failures prohibit further response writes.
    // Caller-mode parser failures leave response generation under handler control.
    //
    bool ResponseClosed() const
    {
        return m_error && (m_options.errorResponse == ParserErrorResponse::Automatic ||
                           m_parseError == 0);
    }

    io::Descriptor& m_desc;
    Context*        m_ctx;
    Cooperator*     m_co;
    time::Interval  m_timeout;

    size_t          m_sendLen;

    Phase           m_phase;
    int64_t         m_contentLength;
    bool            m_chunkedBody;
    size_t          m_bodyRemaining;

    RequestLine     m_requestLine;
    Chunk           m_chunk;
    uint32_t        m_requestLineEpoch; // m_bufEpoch when the request line was parsed
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

    int             m_parseError;
    bool            m_responseBegun;
    size_t          m_headerCount;
    size_t          m_headerBytes;
    bool            m_haveContentLength;
    bool            m_haveTransferEncoding;
    ServerParserOptions m_options;
    int             m_error;
    uint64_t        m_fieldNumber;
    uint8_t         m_fieldState;
    uint8_t         m_tokenLength;
    uint8_t         m_tokenMatches;
    bool            m_lastTokenChunked;
    bool            m_valueStarted;
    bool            m_needChunkCrlf;
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
    static constexpr bool kSpliceable = Transport::kSpliceable;

    // Trailing bytes needed for Allocate: recv buffer + send buffer.
    //
    static constexpr size_t ExtraBytes(
        size_t recvBufSize = ConnectionBase::DEFAULT_BUFFER_SIZE,
        size_t sendBufSize = ConnectionBase::DEFAULT_SEND_BUFFER_SIZE)
    {
        return recvBufSize + sendBufSize;
    }

    Connection(Transport transport, Context* ctx, Cooperator* co,
               size_t recvBufSize = ConnectionBase::DEFAULT_BUFFER_SIZE,
               size_t sendBufSize = ConnectionBase::DEFAULT_SEND_BUFFER_SIZE,
               time::Interval timeout = std::chrono::seconds(30),
               ServerParserOptions options = {})
    : ConnectionImpl<Connection<Transport>>(transport.Descriptor(), ctx, co, timeout, options)
    , m_transport(transport)
    , m_recvBufSize(recvBufSize)
    , m_sendBufSize(sendBufSize)
    { assert(recvBufSize >= 2 && sendBufSize > 0); }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

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

