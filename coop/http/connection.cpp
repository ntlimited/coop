#include "connection.h"
#include "response_constants.h"
#include "transport.h"
#include "tls_transport.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <strings.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/self.h"

namespace coop
{
namespace http
{

template<typename Derived>
ConnectionImpl<Derived>::ConnectionImpl(io::Descriptor& desc, Context* ctx, Cooperator* co,
                                        time::Interval timeout)
: m_desc(desc)
, m_ctx(ctx)
, m_co(co)
, m_timeout(timeout)
, m_bufLen(0)
, m_parsePos(0)
, m_sendLen(0)
, m_phase(REQUEST_LINE)
, m_contentLength(-1)
, m_chunkedBody(false)
, m_bodyRemaining(0)
, m_requestLine{}
, m_chunk{}
, m_requestLineParsed(false)
, m_chunkedDone(false)
, m_valueConsumed(true)
, m_pendingContentLength(false)
, m_pendingTransferEncoding(false)
, m_pendingConnection(false)
, m_chunkedHeadersPending(false)
, m_chunkedStatus(0)
, m_chunkedContentType(nullptr)
, m_keepAlive(true)
, m_clientClose(false)
, m_sendError(false)
{
}

template<typename Derived>
void ConnectionImpl<Derived>::Reset()
{
    Compact();

    m_parsePos          = 0;
    m_sendLen           = 0;
    m_phase             = REQUEST_LINE;
    m_contentLength     = -1;
    m_chunkedBody       = false;
    m_bodyRemaining     = 0;
    m_requestLine       = {};
    m_chunk             = {};
    m_requestLineParsed = false;
    m_chunkedDone       = false;
    m_valueConsumed     = true;
    m_pendingContentLength     = false;
    m_pendingTransferEncoding  = false;
    m_pendingConnection        = false;
    m_chunkedHeadersPending    = false;
    m_chunkedStatus            = 0;
    m_chunkedContentType       = nullptr;
    m_clientClose              = false;
    m_sendError                = false;
}

// -------------------------------------------------------------------------------------
// Buffer management
// -------------------------------------------------------------------------------------

template<typename Derived>
int ConnectionImpl<Derived>::RecvMore()
{
    if (m_ctx->IsKilled()) return -1;

    if (m_bufLen >= RecvBufSize())
    {
        Compact();
        if (m_bufLen >= RecvBufSize()) return 0;
    }

    int n = TransportRecv(RecvBuf() +m_bufLen, RecvBufSize() - m_bufLen, 0, m_timeout);

    if (m_ctx->IsKilled()) return -1;
    if (n <= 0) return -1;

    m_bufLen += n;
    return n;
}

template<typename Derived>
void ConnectionImpl<Derived>::Compact()
{
    if (m_parsePos == 0) return;

    size_t remaining = m_bufLen - m_parsePos;
    if (remaining > 0)
    {
        memmove(RecvBuf(), RecvBuf() +m_parsePos, remaining);
    }
    m_bufLen = remaining;
    m_parsePos = 0;
}

// -------------------------------------------------------------------------------------
// Phase 1: Request line
// -------------------------------------------------------------------------------------

template<typename Derived>
RequestLine* ConnectionImpl<Derived>::GetRequestLine()
{
    if (m_requestLineParsed)
    {
        return m_requestLine.method.empty() ? nullptr : &m_requestLine;
    }
    m_requestLineParsed = true;

    while (true)
    {
        size_t avail = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
        char* cr = avail > 0
            ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', avail))
            : nullptr;

        if (cr && cr + 1 < RecvBuf() + m_bufLen && cr[1] == '\n')
        {
            if (!ParseRequestLine()) return nullptr;
            m_phase = ARGS;
            return &m_requestLine;
        }

        Compact();
        if (RecvMore() <= 0) return nullptr;
    }
}

template<typename Derived>
bool ConnectionImpl<Derived>::ParseRequestLine()
{
    char* base = RecvBuf();
    char* end = base + m_bufLen;
    char* p = base + m_parsePos;

    // Find method end (first space)
    //
    char* sp = static_cast<char*>(memchr(p, ' ', end - p));
    if (!sp) return false;

    m_requestLine.method = std::string_view(p, sp - p);
    p = sp + 1;
    size_t pathStart = p - base;

    // Path end — multi-delimiter, keep byte loop (typically < 20 chars)
    //
    size_t i = pathStart;
    while (i < m_bufLen && base[i] != '?' && base[i] != ' ' && base[i] != '\r') i++;
    if (i >= m_bufLen) return false;

    m_requestLine.path = std::string_view(base + pathStart, i - pathStart);

    if (RecvBuf()[i] == '?')
    {
        m_parsePos = i + 1;
    }
    else
    {
        m_parsePos = i;
    }

    return true;
}

// -------------------------------------------------------------------------------------
// Phase advancement
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::AdvanceToPhase(Phase target)
{
    while (m_phase < target)
    {
        switch (m_phase)
        {
            case REQUEST_LINE:
                if (!GetRequestLine()) return false;
                break;

            case ARGS:
                SkipArgs();
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

template<typename Derived>
void ConnectionImpl<Derived>::SkipToHeaders()
{
    while (true)
    {
        size_t avail = m_bufLen - m_parsePos;
        char* nl = avail > 0
            ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\n', avail))
            : nullptr;

        if (nl)
        {
            m_parsePos = (nl - RecvBuf()) + 1;
            m_phase = HEADERS;
            m_valueConsumed = true;
            return;
        }

        Compact();
        if (RecvMore() <= 0)
        {
            m_phase = DONE;
            return;
        }
    }
}

// -------------------------------------------------------------------------------------
// Phase 2: GET args (query string)
// -------------------------------------------------------------------------------------

template<typename Derived>
const char* ConnectionImpl<Derived>::NextArgName()
{
    if (m_phase < ARGS)
    {
        if (!AdvanceToPhase(ARGS)) return nullptr;
    }
    if (m_phase > ARGS) return nullptr;

    if (!m_valueConsumed)
    {
        SkipArgValue();
    }

    if (m_parsePos >= m_bufLen || RecvBuf()[m_parsePos] == ' ' || RecvBuf()[m_parsePos] == '\r')
    {
        SkipToHeaders();
        return nullptr;
    }

    size_t nameStart = m_parsePos;

    while (true)
    {
        for (size_t i = (nameStart == m_parsePos ? m_parsePos : m_parsePos); i < m_bufLen; i++)
        {
            char c = RecvBuf()[i];
            if (c == '=' || c == '&' || c == ' ' || c == '\r')
            {
                RecvBuf()[i] = '\0';
                const char* name = RecvBuf() +nameStart;
                m_parsePos = i;

                if (c == '=')
                {
                    m_parsePos++;
                    m_valueConsumed = false;
                }
                else
                {
                    if (c == '&') m_parsePos++;
                    m_valueConsumed = true;
                }

                return name;
            }
        }

        Compact();
        nameStart = m_parsePos;
        if (RecvMore() <= 0) return nullptr;
    }
}

template<typename Derived>
Chunk* ConnectionImpl<Derived>::ReadArgValue()
{
    if (m_valueConsumed) return nullptr;

    size_t valueStart = m_parsePos;

    for (size_t i = m_parsePos; i < m_bufLen; i++)
    {
        char c = RecvBuf()[i];
        if (c == '&' || c == ' ' || c == '\r')
        {
            m_chunk.data = RecvBuf() +valueStart;
            m_chunk.size = i - valueStart;
            m_chunk.complete = true;

            m_parsePos = i;
            if (c == '&') m_parsePos++;
            m_valueConsumed = true;
            return &m_chunk;
        }
    }

    size_t available = m_bufLen - valueStart;
    if (available > 0)
    {
        m_chunk.data = RecvBuf() +valueStart;
        m_chunk.size = available;
        m_chunk.complete = false;

        m_parsePos = m_bufLen;
        RecvMore();
        return &m_chunk;
    }

    if (RecvMore() <= 0)
    {
        m_valueConsumed = true;
        return nullptr;
    }

    return ReadArgValue();
}

template<typename Derived>
void ConnectionImpl<Derived>::SkipArgValue()
{
    if (m_valueConsumed) return;

    while (true)
    {
        for (size_t i = m_parsePos; i < m_bufLen; i++)
        {
            char c = RecvBuf()[i];
            if (c == '&' || c == ' ' || c == '\r')
            {
                m_parsePos = i;
                if (c == '&') m_parsePos++;
                m_valueConsumed = true;
                return;
            }
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
void ConnectionImpl<Derived>::SkipArgs()
{
    if (m_phase < ARGS)
    {
        if (!AdvanceToPhase(ARGS)) return;
    }
    if (m_phase != ARGS) return;

    SkipToHeaders();
}

// -------------------------------------------------------------------------------------
// Phase 3: Headers
// -------------------------------------------------------------------------------------

template<typename Derived>
const char* ConnectionImpl<Derived>::NextHeaderName()
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
            size_t avail = m_bufLen - m_parsePos;
            char* colon = avail > 0
                ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, ':', avail))
                : nullptr;

            if (colon)
            {
                // Check for \r before the colon (malformed)
                //
                char* cr = static_cast<char*>(
                    memchr(RecvBuf() + m_parsePos, '\r',
                           colon - (RecvBuf() + m_parsePos)));
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

            // No colon — check for \r
            //
            char* cr = avail > 0
                ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', avail))
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
Chunk* ConnectionImpl<Derived>::ReadHeaderValue()
{
    if (m_valueConsumed) return nullptr;

    size_t valueStart = m_parsePos;

    size_t avail = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
    char* cr = avail > 0
        ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', avail))
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
                m_clientClose = true;
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
        m_chunk.data = RecvBuf() +valueStart;
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
void ConnectionImpl<Derived>::SkipHeaderValue()
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
        size_t avail = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
        char* cr = avail > 0
            ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', avail))
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
void ConnectionImpl<Derived>::SkipHeaders()
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
// Phase 4: Body
// -------------------------------------------------------------------------------------

template<typename Derived>
int64_t ConnectionImpl<Derived>::ContentLength()
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
Chunk* ConnectionImpl<Derived>::ReadBody()
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
    m_chunk.data = RecvBuf() +m_parsePos;
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
void ConnectionImpl<Derived>::SkipBody()
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
Chunk* ConnectionImpl<Derived>::ReadChunkedBody()
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
        m_chunk.data = RecvBuf() +m_parsePos;
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
        size_t avail = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
        char* cr = avail > 0
            ? static_cast<char*>(memchr(RecvBuf() + m_parsePos, '\r', avail))
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
                chunkSize += c & 0xF;
                if (c > '9') c += 9;
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
bool ConnectionImpl<Derived>::Append(const void* data, size_t size)
{
    if (m_sendError) return false;

    // Data larger than the entire send buffer: flush what we have, then send directly
    //
    if (size > SendBufSize())
    {
        if (!Flush()) return false;
        return SendRaw(data, size);
    }

    // Would overflow: flush first, then copy
    //
    if (m_sendLen + size > SendBufSize())
    {
        if (!Flush()) return false;
    }

    memcpy(SendBuf() + m_sendLen, data, size);
    m_sendLen += size;
    return true;
}

template<typename Derived>
bool ConnectionImpl<Derived>::Flush()
{
    if (m_sendError) return false;
    if (m_sendLen == 0) return true;

    bool ok = SendRaw(SendBuf(), m_sendLen);
    m_sendLen = 0;
    return ok;
}

template<typename Derived>
bool ConnectionImpl<Derived>::AppendUInt(size_t val)
{
    // Hand-rolled itoa: max 20 digits for uint64_t. Write backwards then memcpy forward.
    //
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
bool ConnectionImpl<Derived>::AppendHex(size_t val)
{
    static constexpr char HEX[] = "0123456789abcdef";
    char tmp[16];
    int pos = sizeof(tmp);

    if (val == 0)
    {
        tmp[--pos] = '0';
    }
    else
    {
        while (val > 0)
        {
            tmp[--pos] = HEX[val & 0xf];
            val >>= 4;
        }
    }

    return Append(tmp + pos, sizeof(tmp) - pos);
}

template<typename Derived>
template<size_t N>
bool ConnectionImpl<Derived>::AppendLiteral(const char (&s)[N])
{
    return Append(s, N - 1);
}

template<typename Derived>
bool ConnectionImpl<Derived>::AppendConnectionTrailer()
{
    if (m_keepAlive && !m_clientClose)
    {
        return AppendLiteral(response::CONN_KEEP_ALIVE);
    }
    return AppendLiteral(response::CONN_CLOSE);
}

// -------------------------------------------------------------------------------------
// Response
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::SendRaw(const void* data, size_t size)
{
    assert(!m_sendError);

    int result = TransportSendAll(data, size);
    if (result <= 0 || static_cast<size_t>(result) != size)
    {
        m_sendError = true;
        return false;
    }
    return true;
}

// Append the common header block: status line, Content-Type, Content-Length, Connection.
//
template<typename Derived>
bool ConnectionImpl<Derived>::SendHeaders(int status, const char* contentType,
                                           size_t contentLength)
{
    assert(!m_sendError);

    auto sl = response::StatusLine(status);
    if (!Append(sl.data, sl.size)) return false;
    if (!AppendLiteral(response::CONTENT_TYPE)) return false;
    if (!Append(contentType, strlen(contentType))) return false;
    if (!AppendLiteral(response::CONTENT_LENGTH)) return false;
    if (!AppendUInt(contentLength)) return false;
    if (!AppendLiteral(response::CRLF)) return false;
    if (!AppendConnectionTrailer()) return false;
    return Flush();
}

template<typename Derived>
bool ConnectionImpl<Derived>::Send(int status, const char* contentType,
                                    const void* body, size_t size)
{
    assert(!m_sendError);

    auto sl = response::StatusLine(status);
    if (!Append(sl.data, sl.size)) return false;
    if (!AppendLiteral(response::CONTENT_TYPE)) return false;
    if (!Append(contentType, strlen(contentType))) return false;
    if (!AppendLiteral(response::CONTENT_LENGTH)) return false;
    if (!AppendUInt(size)) return false;
    if (!AppendLiteral(response::CRLF)) return false;
    if (!AppendConnectionTrailer()) return false;

    if (size == 0)
    {
        return Flush();
    }

    // Small body: coalesce headers + body into one send
    //
    if (m_sendLen + size <= SendBufSize())
    {
        if (!Append(body, size)) return false;
        return Flush();
    }

    // Large body: flush headers, then send body directly
    //
    if (!Flush()) return false;
    return SendRaw(body, size);
}

template<typename Derived>
bool ConnectionImpl<Derived>::Send(int status, const char* contentType,
                                    const std::string& body)
{
    return Send(status, contentType, body.data(), body.size());
}

template<typename Derived>
bool ConnectionImpl<Derived>::BeginChunked(int status, const char* contentType)
{
    assert(!m_sendError);

    m_chunkedHeadersPending = true;
    m_chunkedStatus = status;
    m_chunkedContentType = contentType;
    return true;
}

// Append chunked response headers into the write buffer (deferred until first chunk).
//
template<typename Derived>
bool ConnectionImpl<Derived>::SendChunk(const void* data, size_t size)
{
    assert(!m_sendError);
    if (size == 0) return false;

    if (m_chunkedHeadersPending)
    {
        m_chunkedHeadersPending = false;

        auto sl = response::StatusLine(m_chunkedStatus);
        if (!Append(sl.data, sl.size)) return false;
        if (!AppendLiteral(response::CONTENT_TYPE)) return false;
        if (!Append(m_chunkedContentType, strlen(m_chunkedContentType))) return false;
        if (!AppendLiteral(response::CRLF)) return false;
        if (!AppendLiteral(response::TRANSFER_ENCODING_CHUNKED)) return false;
        if (!AppendConnectionTrailer()) return false;
    }

    if (!AppendHex(size)) return false;
    if (!AppendLiteral(response::CRLF)) return false;
    if (!Append(data, size)) return false;
    if (!AppendLiteral(response::CRLF)) return false;
    return Flush();
}

template<typename Derived>
bool ConnectionImpl<Derived>::EndChunked()
{
    assert(!m_sendError);

    if (m_chunkedHeadersPending)
    {
        m_chunkedHeadersPending = false;

        auto sl = response::StatusLine(m_chunkedStatus);
        if (!Append(sl.data, sl.size)) return false;
        if (!AppendLiteral(response::CONTENT_TYPE)) return false;
        if (!Append(m_chunkedContentType, strlen(m_chunkedContentType))) return false;
        if (!AppendLiteral(response::CRLF)) return false;
        if (!AppendLiteral(response::TRANSFER_ENCODING_CHUNKED)) return false;
        if (!AppendConnectionTrailer()) return false;
    }

    if (!AppendLiteral(response::CHUNKED_TERMINATOR)) return false;
    return Flush();
}

template<typename Derived>
bool ConnectionImpl<Derived>::EndChunked(const void* lastChunkData, size_t lastChunkSize)
{
    assert(!m_sendError);
    if (lastChunkSize == 0) return EndChunked();

    if (m_chunkedHeadersPending)
    {
        m_chunkedHeadersPending = false;

        auto sl = response::StatusLine(m_chunkedStatus);
        if (!Append(sl.data, sl.size)) return false;
        if (!AppendLiteral(response::CONTENT_TYPE)) return false;
        if (!Append(m_chunkedContentType, strlen(m_chunkedContentType))) return false;
        if (!AppendLiteral(response::CRLF)) return false;
        if (!AppendLiteral(response::TRANSFER_ENCODING_CHUNKED)) return false;
        if (!AppendConnectionTrailer()) return false;
    }

    if (!AppendHex(lastChunkSize)) return false;
    if (!AppendLiteral(response::CRLF)) return false;
    if (!Append(lastChunkData, lastChunkSize)) return false;
    if (!AppendLiteral(response::CRLF)) return false;
    if (!AppendLiteral(response::CHUNKED_TERMINATOR)) return false;
    return Flush();
}

template<typename Derived>
bool ConnectionImpl<Derived>::Sendfile(int fileFd, off_t offset, size_t count)
{
    assert(!m_sendError);

    // Flush any buffered headers before sendfile
    //
    if (!Flush()) return false;

    int result = TransportSendfileAll(fileFd, offset, count);
    if (result <= 0 || static_cast<size_t>(result) != count)
    {
        m_sendError = true;
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------------------
// Explicit template instantiations for known transport types
// -------------------------------------------------------------------------------------

template struct ConnectionImpl<Connection<PlaintextTransport>>;
template struct ConnectionImpl<Connection<TlsTransport>>;

} // end namespace coop::http
} // end namespace coop
