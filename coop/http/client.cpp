#include "client.h"
#include "connection.h"
#include "transport.h"
#include "tls_transport.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <strings.h>

namespace coop
{
namespace http
{

template<typename Derived>
ClientConnectionImpl<Derived>::ClientConnectionImpl(
    io::Descriptor& desc, const char* host, time::Interval timeout)
: m_desc(desc)
, m_host(host)
, m_timeout(timeout)
, m_bufLen(0)
, m_parsePos(0)
, m_sendLen(0)
, m_phase(RESPONSE_LINE)
, m_contentLength(-1)
, m_chunkedBody(false)
, m_bodyRemaining(0)
, m_responseLine{}
, m_chunk{}
, m_responseLineParsed(false)
, m_chunkedDone(false)
, m_valueConsumed(true)
, m_pendingContentLength(false)
, m_pendingTransferEncoding(false)
, m_pendingConnection(false)
, m_keepAlive(true)
, m_serverClose(false)
{
}

template<typename Derived>
void ClientConnectionImpl<Derived>::Reset()
{
    Compact();

    m_parsePos              = 0;
    m_sendLen               = 0;
    m_phase                 = RESPONSE_LINE;
    m_contentLength         = -1;
    m_chunkedBody           = false;
    m_bodyRemaining         = 0;
    m_responseLine          = {};
    m_chunk                 = {};
    m_responseLineParsed    = false;
    m_chunkedDone           = false;
    m_valueConsumed         = true;
    m_pendingContentLength  = false;
    m_pendingTransferEncoding = false;
    m_pendingConnection     = false;
    m_serverClose           = false;
}

// -------------------------------------------------------------------------------------
// Buffer management
// -------------------------------------------------------------------------------------

template<typename Derived>
int ClientConnectionImpl<Derived>::RecvMore()
{
    if (m_bufLen >= RecvBufSize())
    {
        Compact();
        if (m_bufLen >= RecvBufSize()) return 0;
    }

    int n = TransportRecv(RecvBuf() + m_bufLen, RecvBufSize() - m_bufLen, 0, m_timeout);
    if (n <= 0) return -1;

    m_bufLen += n;
    return n;
}

template<typename Derived>
void ClientConnectionImpl<Derived>::Compact()
{
    if (m_parsePos == 0) return;

    size_t remaining = m_bufLen - m_parsePos;
    if (remaining > 0)
    {
        memmove(RecvBuf(), RecvBuf() + m_parsePos, remaining);
    }
    m_bufLen = remaining;
    m_parsePos = 0;
}

// -------------------------------------------------------------------------------------
// Phase 1: Response status line — "HTTP/1.1 200 OK\r\n"
// -------------------------------------------------------------------------------------

template<typename Derived>
ResponseLine* ClientConnectionImpl<Derived>::GetResponseLine()
{
    if (m_responseLineParsed)
    {
        return m_responseLine.status > 0 ? &m_responseLine : nullptr;
    }
    m_responseLineParsed = true;

    while (true)
    {
        size_t searchLen = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
        char* cr = searchLen > 0
            ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', searchLen))
            : nullptr;

        if (cr && cr + 1 < RecvBuf() + m_bufLen && cr[1] == '\n')
        {
            if (!ParseResponseLine()) return nullptr;
            m_parsePos = (cr - RecvBuf()) + 2;
            m_phase = HEADERS;
            return &m_responseLine;
        }

        Compact();
        if (RecvMore() <= 0) return nullptr;
    }
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::ParseResponseLine()
{
    // Format: "HTTP/1.x SSS Reason\r\n"
    //
    char* base = RecvBuf();
    char* p = base + m_parsePos;
    char* end = base + m_bufLen;

    char* lineEnd = static_cast<char*>(memchr(p, '\r', end - p));
    if (!lineEnd) return false;

    // Skip "HTTP/1.x "
    //
    char* sp = static_cast<char*>(memchr(p, ' ', lineEnd - p));
    if (!sp) return false;
    p = sp + 1;

    // Parse 3-digit status code
    //
    if (p + 3 > lineEnd) return false;
    int status = 0;
    for (int d = 0; d < 3; d++)
    {
        char c = p[d];
        if (c < '0' || c > '9') return false;
        status = status * 10 + (c - '0');
    }
    m_responseLine.status = status;
    p += 3;

    if (p < lineEnd && *p == ' ') p++;
    m_responseLine.reason = std::string_view(p, lineEnd - p);

    return true;
}

// -------------------------------------------------------------------------------------
// Phase advancement
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ClientConnectionImpl<Derived>::AdvanceToPhase(Phase target)
{
    while (m_phase < target)
    {
        switch (m_phase)
        {
            case RESPONSE_LINE:
                if (!GetResponseLine()) return false;
                break;

            case HEADERS:
                SkipHeaders();
                break;

            case BODY:
                SkipBody();
                break;

            case DONE:
                return false;
        }
    }
    return true;
}

// -------------------------------------------------------------------------------------
// Phase 2: Headers
// -------------------------------------------------------------------------------------

template<typename Derived>
const char* ClientConnectionImpl<Derived>::NextHeaderName()
{
    if (m_phase < HEADERS)
    {
        if (!AdvanceToPhase(HEADERS)) return nullptr;
    }
    if (m_phase > HEADERS) return nullptr;

    if (!m_valueConsumed)
    {
        SkipHeaderValue();
    }

    while (true)
    {
        while (m_bufLen - m_parsePos < 2)
        {
            Compact();
            if (RecvMore() <= 0) return nullptr;
        }

        if (RecvBuf()[m_parsePos] == '\r' && RecvBuf()[m_parsePos + 1] == '\n')
        {
            m_parsePos += 2;
            m_phase = BODY;
            return nullptr;
        }

        size_t nameStart = m_parsePos;

        while (true)
        {
            size_t searchLen = m_bufLen - m_parsePos;
            char* colon = searchLen > 0
                ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, ':', searchLen))
                : nullptr;

            if (colon)
            {
                char* cr = static_cast<char*>(
                    memchr(RecvBuf() + m_parsePos, '\r', colon - (RecvBuf() + m_parsePos)));
                if (cr)
                {
                    m_phase = DONE;
                    return nullptr;
                }

                size_t i = colon - RecvBuf();
                size_t nameEnd = i;
                RecvBuf()[i] = '\0';
                const char* name = RecvBuf() + nameStart;
                m_parsePos = i + 1;

                while (m_parsePos < m_bufLen && RecvBuf()[m_parsePos] == ' ')
                {
                    m_parsePos++;
                }

                size_t nameLen = nameEnd - nameStart;
                if (nameLen == 14 &&
                    strncasecmp(name, "content-length", 14) == 0)
                {
                    m_pendingContentLength = true;
                }
                if (nameLen == 17 &&
                    strncasecmp(name, "transfer-encoding", 17) == 0)
                {
                    m_pendingTransferEncoding = true;
                }
                if (nameLen == 10 &&
                    strncasecmp(name, "connection", 10) == 0)
                {
                    m_pendingConnection = true;
                }

                m_valueConsumed = false;
                return name;
            }

            char* cr = searchLen > 0
                ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', searchLen))
                : nullptr;
            if (cr)
            {
                m_phase = DONE;
                return nullptr;
            }

            Compact();
            nameStart = m_parsePos;
            if (RecvMore() <= 0) return nullptr;
        }
    }
}

template<typename Derived>
Chunk* ClientConnectionImpl<Derived>::ReadHeaderValue()
{
    if (m_valueConsumed) return nullptr;

    size_t valueStart = m_parsePos;
    size_t searchLen = m_bufLen - m_parsePos;
    char* cr = searchLen > 0
        ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', searchLen))
        : nullptr;

    if (cr)
    {
        size_t i = cr - RecvBuf();
        m_chunk.data = RecvBuf() + valueStart;
        m_chunk.size = i - valueStart;
        m_chunk.complete = true;

        if (m_pendingContentLength)
        {
            m_contentLength = 0;
            const char* p = static_cast<const char*>(m_chunk.data);
            for (size_t j = 0; j < m_chunk.size; j++)
            {
                if (p[j] >= '0' && p[j] <= '9')
                {
                    m_contentLength = m_contentLength * 10 + (p[j] - '0');
                }
            }
            m_pendingContentLength = false;
        }
        if (m_pendingTransferEncoding)
        {
            if (m_chunk.size >= 7 &&
                strncasecmp(static_cast<const char*>(m_chunk.data), "chunked", 7) == 0)
            {
                m_chunkedBody = true;
            }
            m_pendingTransferEncoding = false;
        }
        if (m_pendingConnection)
        {
            if (m_chunk.size >= 5 &&
                strncasecmp(static_cast<const char*>(m_chunk.data), "close", 5) == 0)
            {
                m_serverClose = true;
            }
            m_pendingConnection = false;
        }

        m_parsePos = i;
        if (m_parsePos + 1 < m_bufLen && RecvBuf()[m_parsePos + 1] == '\n')
        {
            m_parsePos += 2;
        }
        m_valueConsumed = true;
        return &m_chunk;
    }

    size_t available = m_bufLen - valueStart;
    if (available > 0)
    {
        m_chunk.data = RecvBuf() + valueStart;
        m_chunk.size = available;
        m_chunk.complete = false;

        m_parsePos = m_bufLen;
        RecvMore();
        return &m_chunk;
    }

    if (RecvMore() <= 0)
    {
        m_pendingContentLength = false;
        m_pendingTransferEncoding = false;
        m_pendingConnection = false;
        m_valueConsumed = true;
        return nullptr;
    }

    return ReadHeaderValue();
}

template<typename Derived>
void ClientConnectionImpl<Derived>::SkipHeaderValue()
{
    if (m_valueConsumed) return;

    if (m_pendingContentLength || m_pendingTransferEncoding || m_pendingConnection)
    {
        while (true)
        {
            Chunk* c = ReadHeaderValue();
            if (!c || c->complete) break;
        }
        return;
    }

    while (true)
    {
        size_t searchLen = m_bufLen - m_parsePos;
        char* cr = searchLen > 0
            ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', searchLen))
            : nullptr;

        if (cr)
        {
            m_parsePos = cr - RecvBuf();
            if (m_parsePos + 1 < m_bufLen && RecvBuf()[m_parsePos + 1] == '\n')
            {
                m_parsePos += 2;
            }
            m_valueConsumed = true;
            return;
        }

        Compact();
        if (RecvMore() <= 0)
        {
            m_valueConsumed = true;
            return;
        }
    }
}

template<typename Derived>
void ClientConnectionImpl<Derived>::SkipHeaders()
{
    if (m_phase < HEADERS)
    {
        if (!AdvanceToPhase(HEADERS)) return;
    }
    if (m_phase != HEADERS) return;

    while (NextHeaderName() != nullptr)
    {
        SkipHeaderValue();
    }
}

// -------------------------------------------------------------------------------------
// Phase 3: Body
// -------------------------------------------------------------------------------------

template<typename Derived>
int64_t ClientConnectionImpl<Derived>::ContentLength()
{
    if (m_contentLength >= 0) return m_contentLength;

    if (m_phase < BODY)
    {
        AdvanceToPhase(BODY);
    }

    if (m_contentLength < 0) m_contentLength = 0;
    return m_contentLength;
}

template<typename Derived>
Chunk* ClientConnectionImpl<Derived>::ReadBody()
{
    if (m_phase < BODY)
    {
        if (!AdvanceToPhase(BODY)) return nullptr;
    }
    if (m_phase != BODY) return nullptr;

    if (m_chunkedBody)
    {
        return ReadChunkedBody();
    }

    if (m_contentLength <= 0)
    {
        m_phase = DONE;
        return nullptr;
    }

    if (m_bodyRemaining == 0 && m_contentLength > 0)
    {
        m_bodyRemaining = static_cast<size_t>(m_contentLength);
    }

    if (m_bodyRemaining == 0)
    {
        m_phase = DONE;
        return nullptr;
    }

    size_t available = m_bufLen - m_parsePos;
    if (available == 0)
    {
        Compact();
        if (RecvMore() <= 0)
        {
            m_phase = DONE;
            return nullptr;
        }
        available = m_bufLen - m_parsePos;
    }

    size_t toDeliver = std::min(available, m_bodyRemaining);
    m_chunk.data = RecvBuf() + m_parsePos;
    m_chunk.size = toDeliver;
    m_bodyRemaining -= toDeliver;
    m_chunk.complete = (m_bodyRemaining == 0);
    m_parsePos += toDeliver;

    if (m_bodyRemaining == 0)
    {
        m_phase = DONE;
    }

    return &m_chunk;
}

template<typename Derived>
void ClientConnectionImpl<Derived>::SkipBody()
{
    if (m_phase < BODY)
    {
        if (!AdvanceToPhase(BODY)) return;
    }
    if (m_phase != BODY) return;

    while (ReadBody() != nullptr) {}
}

// -------------------------------------------------------------------------------------
// Chunked body parsing
// -------------------------------------------------------------------------------------

template<typename Derived>
Chunk* ClientConnectionImpl<Derived>::ReadChunkedBody()
{
    if (m_chunkedDone) return nullptr;

    if (m_bodyRemaining > 0)
    {
        size_t available = m_bufLen - m_parsePos;
        if (available == 0)
        {
            Compact();
            if (RecvMore() <= 0) return nullptr;
            available = m_bufLen - m_parsePos;
        }

        size_t toDeliver = std::min(available, m_bodyRemaining);
        m_chunk.data = RecvBuf() + m_parsePos;
        m_chunk.size = toDeliver;
        m_bodyRemaining -= toDeliver;
        m_parsePos += toDeliver;

        if (m_bodyRemaining == 0)
        {
            while (m_bufLen - m_parsePos < 2)
            {
                Compact();
                if (RecvMore() <= 0)
                {
                    m_chunk.complete = true;
                    m_chunkedDone = true;
                    m_phase = DONE;
                    return &m_chunk;
                }
            }
            m_parsePos += 2;
        }

        m_chunk.complete = (m_bodyRemaining == 0);
        return &m_chunk;
    }

    while (true)
    {
        size_t searchLen = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
        char* cr = searchLen > 0
            ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', searchLen))
            : nullptr;

        if (cr && cr + 1 < RecvBuf() + m_bufLen && cr[1] == '\n')
        {
            size_t i = cr - RecvBuf();
            size_t chunkSize = 0;
            for (size_t j = m_parsePos; j < i; j++)
            {
                char c = RecvBuf()[j];
                if (c == ';') break;

                chunkSize <<= 4;
                if (c >= '0' && c <= '9') chunkSize += c - '0';
                else if (c >= 'a' && c <= 'f') chunkSize += c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') chunkSize += c - 'A' + 10;
            }

            m_parsePos = i + 2;

            if (chunkSize == 0)
            {
                m_chunkedDone = true;
                m_phase = DONE;
                return nullptr;
            }

            m_bodyRemaining = chunkSize;
            return ReadChunkedBody();
        }

        Compact();
        if (RecvMore() <= 0) return nullptr;
    }
}

// -------------------------------------------------------------------------------------
// Write buffer
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ClientConnectionImpl<Derived>::Append(const void* data, size_t size)
{
    if (size > SendBufSize())
    {
        if (!Flush()) return false;
        return SendRaw(data, size);
    }

    if (m_sendLen + size > SendBufSize())
    {
        if (!Flush()) return false;
    }

    memcpy(SendBuf() + m_sendLen, data, size);
    m_sendLen += size;
    return true;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::Flush()
{
    if (m_sendLen == 0) return true;

    bool ok = SendRaw(SendBuf(), m_sendLen);
    m_sendLen = 0;
    return ok;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::AppendUInt(size_t val)
{
    char tmp[20];
    int pos = sizeof(tmp);

    if (val == 0)
    {
        tmp[--pos] = '0';
    }
    else
    {
        while (val > 0)
        {
            tmp[--pos] = '0' + (val % 10);
            val /= 10;
        }
    }

    return Append(tmp + pos, sizeof(tmp) - pos);
}

template<typename Derived>
template<size_t N>
bool ClientConnectionImpl<Derived>::AppendLiteral(const char (&s)[N])
{
    return Append(s, N - 1);
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::SendRaw(const void* data, size_t size)
{
    int result = TransportSendAll(data, size);
    if (result <= 0 || static_cast<size_t>(result) != size)
    {
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------------------
// Request sending
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ClientConnectionImpl<Derived>::SendRequest(
    const char* method, const char* path,
    const char* contentType,
    const void* body, size_t bodySize)
{
    // Request line: "METHOD /path HTTP/1.1\r\n"
    //
    if (!Append(method, strlen(method))) return false;
    if (!AppendLiteral(" ")) return false;
    if (!Append(path, strlen(path))) return false;
    if (!AppendLiteral(" HTTP/1.1\r\n")) return false;

    // Host header (required for HTTP/1.1)
    //
    if (!AppendLiteral("Host: ")) return false;
    if (!Append(m_host, strlen(m_host))) return false;
    if (!AppendLiteral("\r\n")) return false;

    // Content-Type + Content-Length for requests with bodies
    //
    if (body && bodySize > 0 && contentType)
    {
        if (!AppendLiteral("Content-Type: ")) return false;
        if (!Append(contentType, strlen(contentType))) return false;
        if (!AppendLiteral("\r\nContent-Length: ")) return false;
        if (!AppendUInt(bodySize)) return false;
        if (!AppendLiteral("\r\n")) return false;
    }

    if (!AppendLiteral("\r\n")) return false;

    if (body && bodySize > 0)
    {
        if (!Append(body, bodySize)) return false;
    }

    return Flush();
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::Get(const char* path)
{
    return SendRequest("GET", path);
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::Post(
    const char* path, const char* contentType,
    const void* body, size_t bodySize)
{
    return SendRequest("POST", path, contentType, body, bodySize);
}

// -------------------------------------------------------------------------------------
// Explicit template instantiations
// -------------------------------------------------------------------------------------

template struct ClientConnectionImpl<ClientConnection<PlaintextTransport>>;
template struct ClientConnectionImpl<ClientConnection<TlsTransport>>;

} // end namespace coop::http
} // end namespace coop
