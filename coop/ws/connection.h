#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <climits>

#include "types.h"
#include "coop/io/descriptor.h"
#include "coop/time/interval.h"

namespace coop
{

struct Context;
struct Cooperator;

namespace ws
{

// ConnectionBase is the handler-facing interface for WebSocket connections. Handlers receive
// ConnectionBase& (or ConnectionBase*) for transport-agnostic frame processing. Virtual
// dispatch at the handler boundary; the frame parser internals use CRTP for zero-overhead
// buffer access.
//
struct ConnectionBase
{
    static constexpr size_t DEFAULT_RECV_BUFFER_SIZE = 4096;
    static constexpr size_t DEFAULT_SEND_BUFFER_SIZE = 512;

    // A control frame carries at most 125 bytes and is never fragmented on the wire
    // (RFC 6455 5.5). Its payload can still arrive in several borrowed receive spans.
    //
    static constexpr size_t MAX_CONTROL_PAYLOAD = 125;

    // Default admission ceiling on data frames and a fragmented message's total.
    // Callers choose their own limit with SetMaxMessageSize; no payload allocation or
    // message reassembly is performed by the connection.
    //
    static constexpr size_t DEFAULT_MAX_MESSAGE_SIZE = 16 * 1024 * 1024;

    // Close codes recorded when a frame is not the grammar it claims.
    //
    static constexpr uint16_t CLOSE_PROTOCOL_ERROR = 1002;
    static constexpr uint16_t CLOSE_MESSAGE_TOO_BIG = 1009;

    virtual ~ConnectionBase() = default;

    // Pull API. Returns the next frame (or next payload chunk of an in-progress frame).
    // Null = connection closed, error, or peer sent close. Handles buffer boundary spanning
    // via chunk delivery — large payloads are delivered in recv-buffer-sized pieces.
    //
    // Control frames (Ping, Pong, Close) are delivered as-is. The handler decides what to do.
    // Continuation frames report the original message opcode (Text/Binary), not Continuation.
    //
    virtual Frame* NextFrame() = 0;

    // Skip the remaining payload of the current frame (if partially consumed).
    //
    virtual void SkipPayload() = 0;

    // PeerClosed is true after the complete Close payload was delivered or skipped.
    // Error distinguishes transport failure/truncation from a peer Close handshake.
    virtual bool PeerClosed() const = 0;
    virtual int Error() const = 0;

    // Send methods. Return false on send failure.
    //
    virtual bool SendText(const void* data, size_t size) = 0;
    virtual bool SendBinary(const void* data, size_t size) = 0;

    // Explicit data-frame fragmentation. Start with Text/Binary and fin=false, then use
    // Continuation; fin=true ends the message. Control sends may interleave. No buffering
    // or message accumulation: each call sends one frame synchronously.
    virtual bool SendFragment(Opcode opcode, bool fin, const void* data, size_t size) = 0;
    virtual bool SendPing(const void* data = nullptr, size_t size = 0) = 0;
    virtual bool SendPong(const void* data, size_t size) = 0;
    virtual bool Close(uint16_t code = 1000) = 0;

    virtual bool SendError() const = 0;

    // Non-zero once a frame violated the protocol: the close code recorded before
    // refusing further parsing (1002 protocol error, 1009 too big).
    //
    virtual uint16_t ProtocolError() const = 0;

    // Set the data-message ceiling (raise, lower, or SIZE_MAX for the protocol limit).
    // Control frames retain their independent 125-byte protocol limit. Set before parsing.
    //
    virtual void SetMaxMessageSize(size_t bytes) = 0;

    // Default true preserves automatic protocol-error Close replies. False records the
    // error without sending; the handler can inspect ProtocolError() and choose a reply.
    virtual void SetAutoCloseOnError(bool enabled) = 0;

    virtual io::Descriptor& GetDescriptor() = 0;
};

// ConnectionImpl<Derived> is the CRTP frame parser. All parsing state lives here; buffer and
// transport access go through the Derived type (compile-time offset, zero indirection).
//
// Custom transports include detail/connection_impl.hpp; native transports are instantiated in .cpp.
//
template<typename Derived>
struct ConnectionImpl : ConnectionBase
{
    explicit ConnectionImpl(time::Interval timeout);

    Frame* NextFrame() override;
    void SkipPayload() override;
    bool PeerClosed() const override { return m_gotClose && m_payloadRemaining == 0; }
    int Error() const override { return m_recvError; }
    bool SendText(const void* data, size_t size) override;
    bool SendBinary(const void* data, size_t size) override;
    bool SendFragment(Opcode opcode, bool fin, const void* data, size_t size) override;
    bool SendPing(const void* data, size_t size) override;
    bool SendPong(const void* data, size_t size) override;
    bool Close(uint16_t code) override;
    bool SendError() const override { return m_sendError; }
    uint16_t ProtocolError() const override { return m_protocolError; }
    void SetMaxMessageSize(size_t bytes) override { m_maxMessageSize = bytes; }
    void SetAutoCloseOnError(bool enabled) override { m_autoCloseOnError = enabled; }
    io::Descriptor& GetDescriptor() override
    {
        return static_cast<Derived*>(this)->m_transport.Descriptor();
    }

    // Called by Upgrade() to seed the recv buffer with leftover HTTP data.
    //
    void SetInitialRecvData(size_t n) { assert(n <= RecvBufSize()); m_bufLen = n; }

  private:
    // CRTP buffer access
    //
    char* RecvBuf() { return static_cast<Derived*>(this)->m_buf; }
    size_t RecvBufSize() const { return static_cast<const Derived*>(this)->m_recvBufSize; }
    char* SendBuf()
    {
        return static_cast<Derived*>(this)->m_buf
             + static_cast<const Derived*>(this)->m_recvBufSize;
    }
    size_t SendBufSize() const { return static_cast<const Derived*>(this)->m_sendBufSize; }

    // CRTP transport dispatch
    //
    int TransportRecv(void* buf, size_t size, int flags, time::Interval timeout)
    {
        return static_cast<Derived*>(this)->DoRecv(buf, size, flags, timeout);
    }
    int TransportSendAll(const void* buf, size_t size)
    {
        return static_cast<Derived*>(this)->DoSendAll(buf, size);
    }

    // Buffer management
    //
    int RecvMore();
    void Compact();
    size_t Available() const { return m_bufLen - m_parsePos; }

    // Write buffer
    //
    bool Append(const void* data, size_t size);
    bool Flush();

    // Frame parser
    //
    Frame* DeliverPayloadChunk();
    bool SendFrame(Opcode opcode, bool fin, const void* payload, size_t size);

    // Close the connection with `code` and stop parsing. Returns false so the frame
    // validation reads as `if (!Fail(...)) return nullptr;`.
    //
    bool Fail(uint16_t code);

    time::Interval  m_timeout;

    size_t          m_bufLen;
    size_t          m_parsePos;
    size_t          m_sendLen;

    // Frame parser state
    //
    enum ParseState : uint8_t
    {
        HEADER,
        PAYLOAD,
        DONE,
    };

    ParseState      m_parseState;
    Frame           m_frame;
    uint8_t         m_maskKey[4];
    size_t          m_payloadLen;
    size_t          m_payloadRemaining;
    size_t          m_maskOffset;
    Opcode          m_continuationOpcode;
    bool            m_gotClose;
    bool            m_sentClose;
    bool            m_sendError;

    size_t          m_maxMessageSize;
    size_t          m_messageLen;       // bytes of the message being reassembled
    bool            m_messageOpen;      // a fragmented data message is in progress
    uint16_t        m_protocolError;
    int             m_recvError;
    bool            m_sendMessageOpen;
    bool            m_autoCloseOnError;
};

// Connection<Transport> is the final concrete WebSocket connection. The transport parameter
// controls I/O dispatch (PlaintextTransport, TlsTransport) — fully inlined via CRTP.
//
// Layout: [Connection<T> fields] [recv buffer ... recvBufSize] [send buffer ... sendBufSize]
// Allocated via ctx->Allocate<Connection<T>>(ExtraBytes(), ...) from the bump heap.
//
template<typename Transport>
struct Connection final : ConnectionImpl<Connection<Transport>>
{
    static constexpr size_t ExtraBytes(
        size_t recvBufSize = ConnectionBase::DEFAULT_RECV_BUFFER_SIZE,
        size_t sendBufSize = ConnectionBase::DEFAULT_SEND_BUFFER_SIZE)
    {
        return recvBufSize + sendBufSize;
    }

    Connection(Transport transport, Context* ctx,
               size_t recvBufSize = ConnectionBase::DEFAULT_RECV_BUFFER_SIZE,
               size_t sendBufSize = ConnectionBase::DEFAULT_SEND_BUFFER_SIZE,
               time::Interval timeout = std::chrono::seconds(30),
               const char* initialData = nullptr, size_t initialDataSize = 0)
    : ConnectionImpl<Connection<Transport>>(timeout)
    , m_transport(transport)
    , m_recvBufSize(recvBufSize)
    , m_sendBufSize(sendBufSize)
    {
        // At most 12 bytes after the leading two frame-header bytes must fit together.
        assert(recvBufSize >= 12 && sendBufSize > 0 && sendBufSize <= INT_MAX);
        assert(initialDataSize <= recvBufSize && (initialData || initialDataSize == 0));
        (void)ctx;
        if (initialDataSize > 0)
        {
            memcpy(m_buf, initialData, initialDataSize);
            this->SetInitialRecvData(initialDataSize);
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

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

} // namespace coop::ws
} // namespace coop

