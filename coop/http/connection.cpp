#include "connection.h"
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

    if (m_bufLen >= BufSize())
    {
        Compact();
        if (m_bufLen >= BufSize()) return 0;
    }

    int n = TransportRecv(Buf() + m_bufLen, BufSize() - m_bufLen, 0, m_timeout);

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
        memmove(Buf(), Buf() + m_parsePos, remaining);
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
        for (size_t i = m_parsePos; i + 1 < m_bufLen; i++)
        {
            if (Buf()[i] == '\r' && Buf()[i + 1] == '\n')
            {
                if (!ParseRequestLine()) return nullptr;
                m_phase = ARGS;
                return &m_requestLine;
            }
        }

        Compact();
        if (RecvMore() <= 0) return nullptr;
    }
}

template<typename Derived>
bool ConnectionImpl<Derived>::ParseRequestLine()
{
    size_t start = m_parsePos;

    size_t i = start;
    while (i < m_bufLen && Buf()[i] != ' ') i++;
    if (i >= m_bufLen) return false;

    m_requestLine.method = std::string_view(Buf() + start, i - start);

    i++;
    size_t pathStart = i;

    while (i < m_bufLen && Buf()[i] != '?' && Buf()[i] != ' ' && Buf()[i] != '\r') i++;
    if (i >= m_bufLen) return false;

    m_requestLine.path = std::string_view(Buf() + pathStart, i - pathStart);

    if (Buf()[i] == '?')
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
        for (size_t i = m_parsePos; i < m_bufLen; i++)
        {
            if (Buf()[i] == '\n')
            {
                m_parsePos = i + 1;
                m_phase = HEADERS;
                m_valueConsumed = true;
                return;
            }
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

    if (m_parsePos >= m_bufLen || Buf()[m_parsePos] == ' ' || Buf()[m_parsePos] == '\r')
    {
        SkipToHeaders();
        return nullptr;
    }

    size_t nameStart = m_parsePos;

    while (true)
    {
        for (size_t i = (nameStart == m_parsePos ? m_parsePos : m_parsePos); i < m_bufLen; i++)
        {
            char c = Buf()[i];
            if (c == '=' || c == '&' || c == ' ' || c == '\r')
            {
                Buf()[i] = '\0';
                const char* name = Buf() + nameStart;
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
        char c = Buf()[i];
        if (c == '&' || c == ' ' || c == '\r')
        {
            m_chunk.data = Buf() + valueStart;
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
        m_chunk.data = Buf() + valueStart;
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
            char c = Buf()[i];
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

        if (Buf()[m_parsePos] == '\r' && Buf()[m_parsePos + 1] == '\n')
        {
            m_parsePos += 2;
            m_phase = BODY;
            return nullptr;
        }

        size_t nameStart = m_parsePos;

        while (true)
        {
            for (size_t i = m_parsePos; i < m_bufLen; i++)
            {
                if (Buf()[i] == ':')
                {
                    size_t nameEnd = i;
                    Buf()[i] = '\0';
                    const char* name = Buf() + nameStart;
                    m_parsePos = i + 1;

                    while (m_parsePos < m_bufLen && Buf()[m_parsePos] == ' ')
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

                if (Buf()[i] == '\r')
                {
                    m_phase = DONE;
                    return nullptr;
                }
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

    for (size_t i = m_parsePos; i < m_bufLen; i++)
    {
        if (Buf()[i] == '\r')
        {
            m_chunk.data = Buf() + valueStart;
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
            if (m_parsePos + 1 < m_bufLen && Buf()[m_parsePos + 1] == '\n')
            {
                m_parsePos += 2;
            }
            m_valueConsumed = true;
            return &m_chunk;
        }
    }

    size_t available = m_bufLen - valueStart;
    if (available > 0)
    {
        m_chunk.data = Buf() + valueStart;
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
        for (size_t i = m_parsePos; i < m_bufLen; i++)
        {
            if (Buf()[i] == '\r')
            {
                m_parsePos = i;
                if (m_parsePos + 1 < m_bufLen && Buf()[m_parsePos + 1] == '\n')
                {
                    m_parsePos += 2;
                }
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
    m_chunk.data = Buf() + m_parsePos;
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
        m_chunk.data = Buf() + m_parsePos;
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
        for (size_t i = m_parsePos; i + 1 < m_bufLen; i++)
        {
            if (Buf()[i] == '\r' && Buf()[i + 1] == '\n')
            {
                size_t chunkSize = 0;
                for (size_t j = m_parsePos; j < i; j++)
                {
                    char c = Buf()[j];
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
        }

        Compact();
        if (RecvMore() <= 0) return nullptr;
    }
}

// -------------------------------------------------------------------------------------
// Response
// -------------------------------------------------------------------------------------

template<typename Derived>
const char* ConnectionImpl<Derived>::StatusText(int code)
{
    switch (code)
    {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

template<typename Derived>
const char* ConnectionImpl<Derived>::ConnectionHeaderValue() const
{
    return (m_keepAlive && !m_clientClose) ? "keep-alive" : "close";
}

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

template<typename Derived>
bool ConnectionImpl<Derived>::SendWritev(struct iovec* iov, int iovcnt)
{
    assert(!m_sendError);

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++)
    {
        total += iov[i].iov_len;
    }

    int result = TransportWritevAll(iov, iovcnt);
    if (result <= 0 || static_cast<size_t>(result) != total)
    {
        m_sendError = true;
        return false;
    }
    return true;
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendHeaders(int status, const char* contentType,
                                           size_t contentLength)
{
    assert(!m_sendError);

    char hdr[512];
    int len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, StatusText(status), contentType, contentLength, ConnectionHeaderValue());
    assert(len > 0 && static_cast<size_t>(len) < sizeof(hdr));

    return SendRaw(hdr, len);
}

template<typename Derived>
bool ConnectionImpl<Derived>::Send(int status, const char* contentType,
                                    const void* body, size_t size)
{
    assert(!m_sendError);

    char hdr[512];
    int len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, StatusText(status), contentType, size, ConnectionHeaderValue());
    assert(len > 0 && static_cast<size_t>(len) < sizeof(hdr));

    if (size == 0)
    {
        return SendRaw(hdr, len);
    }

    struct iovec iov[2] = {
        { hdr, static_cast<size_t>(len) },
        { const_cast<void*>(body), size },
    };
    return SendWritev(iov, 2);
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

template<typename Derived>
bool ConnectionImpl<Derived>::SendChunk(const void* data, size_t size)
{
    assert(!m_sendError);
    if (size == 0) return false;

    char sizeHdr[32];
    int sizeLen = snprintf(sizeHdr, sizeof(sizeHdr), "%zx\r\n", size);

    if (m_chunkedHeadersPending)
    {
        m_chunkedHeadersPending = false;

        char hdr[512];
        int hdrLen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: %s\r\n"
            "\r\n",
            m_chunkedStatus, StatusText(m_chunkedStatus), m_chunkedContentType,
            ConnectionHeaderValue());
        assert(hdrLen > 0 && static_cast<size_t>(hdrLen) < sizeof(hdr));

        struct iovec iov[4] = {
            { hdr, static_cast<size_t>(hdrLen) },
            { sizeHdr, static_cast<size_t>(sizeLen) },
            { const_cast<void*>(data), size },
            { const_cast<char*>("\r\n"), 2 },
        };
        return SendWritev(iov, 4);
    }

    struct iovec iov[3] = {
        { sizeHdr, static_cast<size_t>(sizeLen) },
        { const_cast<void*>(data), size },
        { const_cast<char*>("\r\n"), 2 },
    };
    return SendWritev(iov, 3);
}

template<typename Derived>
bool ConnectionImpl<Derived>::EndChunked()
{
    assert(!m_sendError);

    if (m_chunkedHeadersPending)
    {
        m_chunkedHeadersPending = false;

        char hdr[512];
        int hdrLen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: %s\r\n"
            "\r\n",
            m_chunkedStatus, StatusText(m_chunkedStatus), m_chunkedContentType,
            ConnectionHeaderValue());
        assert(hdrLen > 0 && static_cast<size_t>(hdrLen) < sizeof(hdr));

        struct iovec iov[2] = {
            { hdr, static_cast<size_t>(hdrLen) },
            { const_cast<char*>("0\r\n\r\n"), 5 },
        };
        return SendWritev(iov, 2);
    }

    return SendRaw("0\r\n\r\n", 5);
}

template<typename Derived>
bool ConnectionImpl<Derived>::EndChunked(const void* lastChunkData, size_t lastChunkSize)
{
    assert(!m_sendError);
    if (lastChunkSize == 0) return EndChunked();

    char sizeHdr[32];
    int sizeLen = snprintf(sizeHdr, sizeof(sizeHdr), "%zx\r\n", lastChunkSize);

    if (m_chunkedHeadersPending)
    {
        m_chunkedHeadersPending = false;

        char hdr[512];
        int hdrLen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: %s\r\n"
            "\r\n",
            m_chunkedStatus, StatusText(m_chunkedStatus), m_chunkedContentType,
            ConnectionHeaderValue());
        assert(hdrLen > 0 && static_cast<size_t>(hdrLen) < sizeof(hdr));

        struct iovec iov[5] = {
            { hdr, static_cast<size_t>(hdrLen) },
            { sizeHdr, static_cast<size_t>(sizeLen) },
            { const_cast<void*>(lastChunkData), lastChunkSize },
            { const_cast<char*>("\r\n"), 2 },
            { const_cast<char*>("0\r\n\r\n"), 5 },
        };
        return SendWritev(iov, 5);
    }

    struct iovec iov[4] = {
        { sizeHdr, static_cast<size_t>(sizeLen) },
        { const_cast<void*>(lastChunkData), lastChunkSize },
        { const_cast<char*>("\r\n"), 2 },
        { const_cast<char*>("0\r\n\r\n"), 5 },
    };
    return SendWritev(iov, 4);
}

template<typename Derived>
bool ConnectionImpl<Derived>::Sendfile(int fileFd, off_t offset, size_t count)
{
    assert(!m_sendError);

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
