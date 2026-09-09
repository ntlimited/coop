#pragma once

#include "../connection.h"


#include <algorithm>
#include <cstring>
#include <cerrno>
#include <climits>
#include <limits>

namespace coop
{
namespace ws
{

// ---------------------------------------------------------------------------
// ConnectionImpl — constructor
// ---------------------------------------------------------------------------

template<typename Derived>
ConnectionImpl<Derived>::ConnectionImpl(time::Interval timeout)
: m_timeout(timeout)
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
, m_maxMessageSize(DEFAULT_MAX_MESSAGE_SIZE)
, m_messageLen(0)
, m_messageOpen(false)
, m_protocolError(0)
, m_recvError(0)
, m_sendMessageOpen(false)
, m_autoCloseOnError(true)
{
}

// ---------------------------------------------------------------------------
// Protocol failure
// ---------------------------------------------------------------------------

// Tell the peer why with a close frame, then stop. A frame header that is not what it
// claims to be leaves no way to find where the next frame starts, so there is nothing to
// resynchronize to — the stream ends here.
//
template<typename Derived>
bool ConnectionImpl<Derived>::Fail(uint16_t code)
{
    if (m_protocolError == 0)
    {
        m_protocolError = code;
        m_recvError = -EPROTO;
        if (m_autoCloseOnError) Close(code);
    }
    m_parseState = DONE;
    m_payloadRemaining = 0;
    return false;
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
    if (space == 0) return m_recvError = -EMSGSIZE;

    int n = TransportRecv(RecvBuf() + m_bufLen, space, 0, m_timeout);
    if (n > 0)
        m_bufLen += static_cast<size_t>(n);
    else
        m_recvError = n < 0 ? n : -ECONNRESET;
    return n;
}

// ---------------------------------------------------------------------------
// Write buffer
// ---------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::Append(const void* data, size_t size)
{
    if (m_sendError) return false;
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
            // The transport reports progress through int; keep every operation representable.
            size_t part = std::min(size, static_cast<size_t>(INT_MAX));
            if (TransportSendAll(p, part) != static_cast<int>(part))
            {
                m_sendError = true;
                return false;
            }
            p += part;
            size -= part;
            continue;
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
    if (m_sendError) return false;
    if (m_sendLen == 0) return true;

    if (TransportSendAll(SendBuf(), m_sendLen) != static_cast<int>(m_sendLen))
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
    if (m_parseState == DONE || (m_gotClose && m_parseState != PAYLOAD))
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
    bool control = (b0 & 0x08) != 0;

    m_parsePos += 2;

    // Everything decidable from the first two bytes is decided before any more of the
    // peer's numbers are read.
    //
    // RSV1-3 are reserved for an extension the handshake negotiated; none is offered
    // here, so a set bit means the peer is framing to a different grammar than this
    // parser reads (RFC 6455 5.2).
    //
    if ((b0 & 0x70) != 0)
    {
        Fail(CLOSE_PROTOCOL_ERROR);
        return nullptr;
    }

    // Opcodes 3-7 and 11-15 are reserved and undefined: a frame using one has no agreed
    // meaning, and guessing at it is how an unknown frame becomes a text message.
    //
    if (op != Opcode::Continuation && op != Opcode::Text && op != Opcode::Binary
        && op != Opcode::Close && op != Opcode::Ping && op != Opcode::Pong)
    {
        Fail(CLOSE_PROTOCOL_ERROR);
        return nullptr;
    }

    // A client-to-server frame must be masked (RFC 6455 5.1). The requirement exists to
    // stop an attacker from steering the bytes a proxy on the path sees; an unmasked
    // frame is not one a conforming client sent.
    //
    if (!masked)
    {
        Fail(CLOSE_PROTOCOL_ERROR);
        return nullptr;
    }

    if (control)
    {
        // A control frame is at most 125 bytes and never fragmented (RFC 6455 5.5), which
        // is what makes echoing a Ping from a fixed buffer safe. Enforcing it here is why
        // a handler can hand frame->data straight back to SendPong.
        //
        if (len7 > MAX_CONTROL_PAYLOAD || !fin)
        {
            Fail(CLOSE_PROTOCOL_ERROR);
            return nullptr;
        }
    }

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
        if (m_payloadLen < 126)
        {
            Fail(CLOSE_PROTOCOL_ERROR);
            return nullptr;
        }
    }
    else // 127
    {
        // The most significant bit of a 64-bit length must be 0 (RFC 6455 5.2). A length
        // with it set is not a size any implementation can honor, and it is the value
        // that overflows arithmetic done on it.
        //
        if ((static_cast<uint8_t>(RecvBuf()[m_parsePos]) & 0x80) != 0)
        {
            Fail(CLOSE_PROTOCOL_ERROR);
            return nullptr;
        }

        m_payloadLen = 0;
        for (int i = 0; i < 8; i++)
        {
            m_payloadLen = (m_payloadLen << 8)
                         | static_cast<uint8_t>(RecvBuf()[m_parsePos + i]);
        }
        m_parsePos += 8;
        if (m_payloadLen <= 65535)
        {
            Fail(CLOSE_PROTOCOL_ERROR);
            return nullptr;
        }
    }

    // Mask key (client → server frames must be masked per RFC 6455 Section 5.1).
    //
    memcpy(m_maskKey, RecvBuf() + m_parsePos, 4);
    m_parsePos += 4;

    if (!control && m_payloadLen > m_maxMessageSize)
    {
        Fail(CLOSE_MESSAGE_TOO_BIG);
        return nullptr;
    }

    // Fragmentation state. A Continuation frame names a message that a preceding
    // non-final data frame opened; without one there is no message for its bytes to join,
    // and delivering them under the last message's opcode makes the peer the author of
    // what type they are. A second data frame while a message is open would interleave
    // two messages in one stream (RFC 6455 5.4).
    //
    if (!control)
    {
        if (op == Opcode::Continuation)
        {
            if (!m_messageOpen)
            {
                Fail(CLOSE_PROTOCOL_ERROR);
                return nullptr;
            }
            if (m_messageLen > m_maxMessageSize
                || m_payloadLen > m_maxMessageSize - m_messageLen)
            {
                Fail(CLOSE_MESSAGE_TOO_BIG);
                return nullptr;
            }
            m_messageLen += m_payloadLen;
        }
        else
        {
            if (m_messageOpen)
            {
                Fail(CLOSE_PROTOCOL_ERROR);
                return nullptr;
            }
            m_messageLen = m_payloadLen;
        }

        if (m_messageLen > m_maxMessageSize)
        {
            Fail(CLOSE_MESSAGE_TOO_BIG);
            return nullptr;
        }
    }

    // A Close frame carries either no payload or a 2-byte code plus a reason; a single
    // byte is half a code (RFC 6455 5.5.1).
    //
    if (op == Opcode::Close && m_payloadLen == 1)
    {
        Fail(CLOSE_PROTOCOL_ERROR);
        return nullptr;
    }

    m_payloadRemaining = m_payloadLen;
    m_maskOffset = 0;

    // Track opcode for continuation frames.
    //
    if (op == Opcode::Continuation)
    {
        m_frame.opcode = m_continuationOpcode;
        if (fin) m_messageOpen = false;
    }
    else if (op == Opcode::Text || op == Opcode::Binary)
    {
        m_frame.opcode = op;
        m_continuationOpcode = op;
        m_messageOpen = !fin;
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
    if (m_sendError || (m_sentClose && opcode != Opcode::Close)) return false;
    if (size > static_cast<size_t>(std::numeric_limits<int64_t>::max())) return false;

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
bool ConnectionImpl<Derived>::SendFragment(Opcode opcode, bool fin,
                                             const void* data, size_t size)
{
    if (opcode == Opcode::Continuation)
    {
        if (!m_sendMessageOpen) return false;
    }
    else if (opcode == Opcode::Text || opcode == Opcode::Binary)
    {
        if (m_sendMessageOpen) return false;
    }
    else return false;

    if (!SendFrame(opcode, fin, data, size)) return false;
    m_sendMessageOpen = !fin;
    return true;
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendText(const void* data, size_t size)
{
    return SendFragment(Opcode::Text, true, data, size);
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendBinary(const void* data, size_t size)
{
    return SendFragment(Opcode::Binary, true, data, size);
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendPing(const void* data, size_t size)
{
    // Refuse rather than assert. The usual Pong payload is a Ping's payload echoed back,
    // so an oversized control frame is something a peer can ask for — aborting the
    // process on it hands the peer the process, and emitting it anyway puts a frame on
    // the wire that no conforming client can read.
    //
    if (size > MAX_CONTROL_PAYLOAD) return false;
    return SendFrame(Opcode::Ping, true, data, size);
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendPong(const void* data, size_t size)
{
    if (size > MAX_CONTROL_PAYLOAD) return false;
    return SendFrame(Opcode::Pong, true, data, size);
}

template<typename Derived>
bool ConnectionImpl<Derived>::Close(uint16_t code)
{
    if (m_sentClose) return !m_sendError;
    m_sentClose = true;
    uint8_t payload[2] = {
        static_cast<uint8_t>(code >> 8),
        static_cast<uint8_t>(code),
    };
    return SendFrame(Opcode::Close, true, payload, 2);
}

} // namespace coop::ws
} // namespace coop
