#include "connection.h"

#include "coop/http/transport.h"
#include "coop/http/tls_transport.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace coop
{
namespace ws
{

// ---------------------------------------------------------------------------
// ConnectionImpl — constructor
// ---------------------------------------------------------------------------

template<typename Derived>
ConnectionImpl<Derived>::ConnectionImpl(io::Descriptor& desc, Context* ctx,
                                        time::Interval timeout)
: m_desc(desc)
, m_ctx(ctx)
, m_timeout(timeout)
, m_bufLen(0)
, m_parsePos(0)
, m_sendLen(0)
, m_parseState(HEADER)
, m_frame{}
, m_maskKey{}
, m_payloadLen(0)
, m_payloadRemaining(0)
, m_maskOffset(0)
, m_continuationOpcode(Opcode::Text)
, m_gotClose(false)
, m_sentClose(false)
, m_sendError(false)
{
}

// ---------------------------------------------------------------------------
// Buffer management
// ---------------------------------------------------------------------------

template<typename Derived>
void ConnectionImpl<Derived>::Compact()
{
    if (m_parsePos == 0) return;

    size_t remaining = m_bufLen - m_parsePos;
    if (remaining > 0)
        memmove(RecvBuf(), RecvBuf() + m_parsePos, remaining);
    m_bufLen = remaining;
    m_parsePos = 0;
}

template<typename Derived>
int ConnectionImpl<Derived>::RecvMore()
{
    if (m_bufLen >= RecvBufSize())
        Compact();

    size_t space = RecvBufSize() - m_bufLen;
    if (space == 0) return 0;

    int n = TransportRecv(RecvBuf() + m_bufLen, space, 0, m_timeout);
    if (n > 0)
        m_bufLen += static_cast<size_t>(n);
    return n;
}

// ---------------------------------------------------------------------------
// Write buffer
// ---------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::Append(const void* data, size_t size)
{
    auto* p = static_cast<const char*>(data);

    while (size > 0)
    {
        size_t space = SendBufSize() - m_sendLen;
        if (space == 0)
        {
            if (!Flush()) return false;
            space = SendBufSize();
        }

        // If the payload is larger than the entire send buffer, flush and send directly.
        //
        if (m_sendLen == 0 && size > space)
        {
            if (TransportSendAll(p, size) < 0)
            {
                m_sendError = true;
                return false;
            }
            return true;
        }

        size_t n = std::min(size, space);
        memcpy(SendBuf() + m_sendLen, p, n);
        m_sendLen += n;
        p += n;
        size -= n;
    }
    return true;
}

template<typename Derived>
bool ConnectionImpl<Derived>::Flush()
{
    if (m_sendLen == 0) return true;

    if (TransportSendAll(SendBuf(), m_sendLen) < 0)
    {
        m_sendError = true;
        return false;
    }
    m_sendLen = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Frame parser — NextFrame()
// ---------------------------------------------------------------------------

template<typename Derived>
Frame* ConnectionImpl<Derived>::NextFrame()
{
    if (m_gotClose || m_parseState == DONE)
        return nullptr;

    // If mid-payload delivery, continue with the next chunk.
    //
    if (m_parseState == PAYLOAD && m_payloadRemaining > 0)
        return DeliverPayloadChunk();

    // Parse frame header. Need at least 2 bytes.
    //
    while (Available() < 2)
    {
        if (RecvMore() <= 0) { m_parseState = DONE; return nullptr; }
    }

    uint8_t b0 = static_cast<uint8_t>(RecvBuf()[m_parsePos]);
    uint8_t b1 = static_cast<uint8_t>(RecvBuf()[m_parsePos + 1]);

    bool fin    = (b0 & 0x80) != 0;
    Opcode op   = static_cast<Opcode>(b0 & 0x0F);
    bool masked = (b1 & 0x80) != 0;
    size_t len7 = b1 & 0x7F;

    m_parsePos += 2;

    // Determine how many additional header bytes we need.
    //
    size_t extraHeader = 0;
    if (len7 == 126) extraHeader = 2;
    else if (len7 == 127) extraHeader = 8;
    if (masked) extraHeader += 4;

    while (Available() < extraHeader)
    {
        if (RecvMore() <= 0) { m_parseState = DONE; return nullptr; }
    }

    // Extended payload length.
    //
    if (len7 <= 125)
    {
        m_payloadLen = len7;
    }
    else if (len7 == 126)
    {
        m_payloadLen = (static_cast<size_t>(
                            static_cast<uint8_t>(RecvBuf()[m_parsePos])) << 8)
                     | static_cast<size_t>(
                            static_cast<uint8_t>(RecvBuf()[m_parsePos + 1]));
        m_parsePos += 2;
    }
    else // 127
    {
        m_payloadLen = 0;
        for (int i = 0; i < 8; i++)
        {
            m_payloadLen = (m_payloadLen << 8)
                         | static_cast<uint8_t>(RecvBuf()[m_parsePos + i]);
        }
        m_parsePos += 8;
    }

    // Mask key (client → server frames must be masked per RFC 6455 Section 5.1).
    //
    if (masked)
    {
        memcpy(m_maskKey, RecvBuf() + m_parsePos, 4);
        m_parsePos += 4;
    }
    else
    {
        memset(m_maskKey, 0, 4);
    }

    m_payloadRemaining = m_payloadLen;
    m_maskOffset = 0;

    // Track opcode for continuation frames.
    //
    if (op == Opcode::Continuation)
    {
        m_frame.opcode = m_continuationOpcode;
    }
    else if (op == Opcode::Text || op == Opcode::Binary)
    {
        m_frame.opcode = op;
        if (!fin) m_continuationOpcode = op;
    }
    else
    {
        m_frame.opcode = op;
    }

    m_frame.fin = fin;

    if (op == Opcode::Close)
        m_gotClose = true;

    // Zero-length payload — return immediately.
    //
    if (m_payloadLen == 0)
    {
        m_frame.data = nullptr;
        m_frame.size = 0;
        m_frame.complete = true;
        m_parseState = HEADER;
        return &m_frame;
    }

    m_parseState = PAYLOAD;
    return DeliverPayloadChunk();
}

// ---------------------------------------------------------------------------
// Frame parser — payload chunk delivery
// ---------------------------------------------------------------------------

template<typename Derived>
Frame* ConnectionImpl<Derived>::DeliverPayloadChunk()
{
    size_t avail = Available();
    if (avail == 0)
    {
        Compact();
        if (RecvMore() <= 0)
        {
            m_parseState = DONE;
            return nullptr;
        }
        avail = Available();
    }

    size_t toDeliver = std::min(avail, m_payloadRemaining);
    char* data = RecvBuf() + m_parsePos;

    // Unmask in-place (client frames are masked; server recv must unmask).
    //
    for (size_t i = 0; i < toDeliver; i++)
        data[i] ^= static_cast<char>(m_maskKey[(m_maskOffset + i) & 3]);
    m_maskOffset = (m_maskOffset + toDeliver) & 3;

    m_frame.data = data;
    m_frame.size = toDeliver;
    m_payloadRemaining -= toDeliver;
    m_frame.complete = (m_payloadRemaining == 0);
    m_parsePos += toDeliver;

    if (m_payloadRemaining == 0)
        m_parseState = HEADER;

    return &m_frame;
}

// ---------------------------------------------------------------------------
// SkipPayload
// ---------------------------------------------------------------------------

template<typename Derived>
void ConnectionImpl<Derived>::SkipPayload()
{
    while (m_parseState == PAYLOAD && m_payloadRemaining > 0)
    {
        size_t avail = Available();
        if (avail == 0)
        {
            Compact();
            if (RecvMore() <= 0) { m_parseState = DONE; return; }
            avail = Available();
        }
        size_t skip = std::min(avail, m_payloadRemaining);
        m_parsePos += skip;
        m_payloadRemaining -= skip;
        m_maskOffset = (m_maskOffset + skip) & 3;
    }
    if (m_payloadRemaining == 0 && m_parseState == PAYLOAD)
        m_parseState = HEADER;
}

// ---------------------------------------------------------------------------
// Send methods
// ---------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::SendFrame(Opcode opcode, bool fin,
                                         const void* payload, size_t size)
{
    if (m_sendError) return false;

    // Server frames are unmasked (RFC 6455 Section 5.1).
    //
    uint8_t header[10];
    size_t headerLen = 2;

    header[0] = (fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode);

    if (size <= 125)
    {
        header[1] = static_cast<uint8_t>(size);
    }
    else if (size <= 65535)
    {
        header[1] = 126;
        header[2] = static_cast<uint8_t>(size >> 8);
        header[3] = static_cast<uint8_t>(size);
        headerLen = 4;
    }
    else
    {
        header[1] = 127;
        for (int i = 0; i < 8; i++)
            header[2 + i] = static_cast<uint8_t>(size >> (56 - 8 * i));
        headerLen = 10;
    }

    if (!Append(header, headerLen)) return false;
    if (size > 0 && !Append(payload, size)) return false;
    return Flush();
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendText(const void* data, size_t size)
{
    return SendFrame(Opcode::Text, true, data, size);
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendBinary(const void* data, size_t size)
{
    return SendFrame(Opcode::Binary, true, data, size);
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendPing(const void* data, size_t size)
{
    assert(size <= 125);  // RFC 6455: control frame payload <= 125 bytes
    return SendFrame(Opcode::Ping, true, data, size);
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendPong(const void* data, size_t size)
{
    assert(size <= 125);
    return SendFrame(Opcode::Pong, true, data, size);
}

template<typename Derived>
bool ConnectionImpl<Derived>::Close(uint16_t code)
{
    if (m_sentClose) return true;
    m_sentClose = true;
    uint8_t payload[2] = {
        static_cast<uint8_t>(code >> 8),
        static_cast<uint8_t>(code),
    };
    return SendFrame(Opcode::Close, true, payload, 2);
}

// ---------------------------------------------------------------------------
// Explicit template instantiation for known transports
// ---------------------------------------------------------------------------

template struct ConnectionImpl<Connection<http::PlaintextTransport>>;
template struct ConnectionImpl<Connection<http::TlsTransport>>;

} // namespace coop::ws
} // namespace coop
