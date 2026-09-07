#include "connection.h"
#include "response_constants.h"
#include "transport.h"
#include "tls_transport.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <strings.h>

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/self.h"
#include "coop/io/detail/yield_budget.h"
#include "coop/io/splice.h"
#include "coop/io/uring.h"
#include "coop/io/write.h"

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
, m_sendLen(0)
, m_phase(REQUEST_LINE)
, m_contentLength(-1)
, m_chunkedBody(false)
, m_bodyRemaining(0)
, m_requestLine{}
, m_chunk{}
, m_requestLineEpoch(0)
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
, m_parseError(0)
, m_responseBegun(false)
, m_headerCount(0)
, m_headerBytes(0)
, m_haveContentLength(false)
, m_haveTransferEncoding(false)
, m_specialValueLen(0)
, m_specialValue{}
{
}

template<typename Derived>
void ConnectionImpl<Derived>::Reset()
{
    // A failed request is not a request boundary: the bytes still buffered belong to a
    // message this parser could not frame, so re-reading them as the next request is the
    // step that turns a framing bug into a smuggled request. Reset is a no-op instead;
    // the connection is already close-framed and its loop exits.
    //
    if (m_parseError != 0) return;

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
    m_responseBegun            = false;
    m_headerCount              = 0;
    m_headerBytes              = 0;
    m_haveContentLength        = false;
    m_haveTransferEncoding     = false;
    m_specialValueLen          = 0;
}

// -------------------------------------------------------------------------------------
// Buffer management
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::RecvAborted() const
{
    return m_ctx->IsKilled();
}

// -------------------------------------------------------------------------------------
// Framing enforcement
// -------------------------------------------------------------------------------------

namespace
{

// Reason phrase and body for the statuses a framing failure can answer with. 431 is not
// in the pre-compiled status table, so its phrase is supplied here rather than falling
// back to the status-class default.
//
struct FailureText
{
    const char* reason;
    const char* body;
};

FailureText TextForFailure(int status)
{
    switch (status)
    {
        case 413: return { "Payload Too Large", "Payload Too Large\n" };
        case 431: return { "Request Header Fields Too Large",
                           "Request Header Fields Too Large\n" };
        default:  return { "Bad Request", "Bad Request\n" };
    }
}

// Hex digit value, or -1 for anything that is not one. `c & 0xF` maps every byte to a
// digit, which is why the chunk-size line needs a real predicate.
//
int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // end anonymous namespace

// End the request: answer the peer once with `status` and make every later parse and
// response call a no-op. A request whose framing cannot be trusted has no safe partial
// interpretation — not the body prefix that did arrive, and not the bytes behind it.
//
template<typename Derived>
bool ConnectionImpl<Derived>::FailRequest(int status)
{
    if (m_parseError != 0) return false;

    m_phase                   = DONE;
    m_valueConsumed           = true;
    m_chunkedDone             = false;
    m_bodyRemaining           = 0;
    m_contentLength           = 0;
    m_keepAlive               = false;
    m_clientClose             = true;
    m_pendingContentLength    = false;
    m_pendingTransferEncoding = false;
    m_pendingConnection       = false;

    // A handler that already began a response owns the bytes on the wire; a status line
    // cannot be spliced into the middle of one. Its response is left truncated and the
    // connection closes, which is the only signal left to give.
    //
    if (!m_responseBegun && !m_sendError)
    {
        FailureText text = TextForFailure(status);
        size_t bodyLen = strlen(text.body);

        if (BeginResponse(status, text.reason)
            && AppendHeader("Content-Type", std::string_view("text/plain"))
            && AppendHeader("Content-Length", bodyLen)
            && EndHeaders())
        {
            SendRawBytes(text.body, bodyLen);
        }
    }

    m_parseError = status;
    return false;
}

// Header and trailer lines are charged against one cumulative budget. The recv buffer
// bounds a single line, but the parser compacts and refills as it scans, so without an
// explicit budget a peer can stream header bytes for as long as it likes.
//
template<typename Derived>
bool ConnectionImpl<Derived>::ChargeHeaderBytes(size_t bytes)
{
    m_headerBytes += bytes;
    if (m_headerBytes > MAX_HEADER_BYTES)
    {
        return FailRequest(431);
    }
    return true;
}

// Accumulate a Content-Length / Transfer-Encoding / Connection value as it streams. A
// value longer than any legitimate form of the three overflows deliberately: the flag
// records it so the completed value is judged malformed rather than silently truncated.
//
template<typename Derived>
void ConnectionImpl<Derived>::CaptureSpecialValue(const char* data, size_t size)
{
    if (m_specialValueLen > SPECIAL_VALUE_MAX) return;   // already marked overflowed

    size_t space = SPECIAL_VALUE_MAX - m_specialValueLen;
    size_t take = size < space ? size : space;
    if (take > 0)
    {
        memcpy(m_specialValue + m_specialValueLen, data, take);
        m_specialValueLen += take;
    }
    if (take < size)
    {
        m_specialValueLen = SPECIAL_VALUE_MAX + 1;   // overflow marker
    }
}

// Decode the completed special value into framing state.
//
template<typename Derived>
bool ConnectionImpl<Derived>::ApplySpecialValue()
{
    bool overflow = m_specialValueLen > SPECIAL_VALUE_MAX;
    size_t len = overflow ? SPECIAL_VALUE_MAX : m_specialValueLen;
    const char* p = m_specialValue;

    // Optional whitespace around a field value is not part of it (RFC 7230 3.2.4).
    //
    while (len > 0 && (*p == ' ' || *p == '\t')) { p++; len--; }
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;

    m_specialValueLen = 0;

    if (m_pendingContentLength)
    {
        m_pendingContentLength = false;

        // Content-Length is 1*DIGIT and nothing else. Skipping non-digits reads
        // "1abc0" as 10, which is a body length the peer never declared — the length a
        // downstream reader computes and the length this one computes then differ.
        //
        if (overflow || len == 0 || len > 19)
        {
            return FailRequest(400);
        }

        int64_t value = 0;
        for (size_t i = 0; i < len; i++)
        {
            if (p[i] < '0' || p[i] > '9')
            {
                return FailRequest(400);
            }
            value = value * 10 + (p[i] - '0');
        }

        // Repeating the header is allowed only when it repeats the same length; two
        // different lengths leave the message with no single boundary.
        //
        if (m_haveContentLength && value != m_contentLength)
        {
            return FailRequest(400);
        }

        m_haveContentLength = true;
        m_contentLength = value;
        return true;
    }

    if (m_pendingTransferEncoding)
    {
        m_pendingTransferEncoding = false;

        // The only transfer coding this parser can frame is `chunked`. Anything else —
        // including a prefix match such as "chunkedfoo" — leaves the body's end unknown.
        //
        if (overflow || len != 7 || strncasecmp(p, "chunked", 7) != 0)
        {
            return FailRequest(400);
        }

        m_haveTransferEncoding = true;
        m_chunkedBody = true;
        return true;
    }

    if (m_pendingConnection)
    {
        m_pendingConnection = false;
        if (!overflow && len == 5 && strncasecmp(p, "close", 5) == 0)
        {
            m_clientClose = true;
        }
        return true;
    }

    return true;
}

// Called once the blank line closing the header block has been consumed.
//
template<typename Derived>
bool ConnectionImpl<Derived>::FinishHeaders()
{
    // Content-Length and Transfer-Encoding are two different answers to "where does this
    // body end". Accepting both lets two readers of the same bytes disagree about the
    // message boundary, and the trailing bytes then become a request nobody sent
    // (RFC 7230 3.3.3). There is no reading that is safe to guess at.
    //
    if (m_haveContentLength && m_haveTransferEncoding)
    {
        return FailRequest(400);
    }
    return true;
}

// -------------------------------------------------------------------------------------
// Phase 1: Request line
// -------------------------------------------------------------------------------------

template<typename Derived>
RequestLine* ConnectionImpl<Derived>::GetRequestLine()
{
    if (m_requestLineParsed)
    {
        if (m_requestLine.method.empty())
        {
            return nullptr;
        }

        // The memoized views point into recv-buffer bytes that Compact() has since
        // discarded — serving them would read reused memory. Callers must copy what
        // they need before header/body parsing (see connection.h).
        //
        if (m_requestLineEpoch != m_bufEpoch)
        {
            assert(false && "RequestLine views invalidated by buffer compaction — "
                            "copy method/path/target before parsing headers");
            return nullptr;
        }
        return &m_requestLine;
    }
    m_requestLineParsed = true;

    while (true)
    {
        size_t avail = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
        char* cr = avail > 0
            ? static_cast<char*>(memchr(Win() + m_parsePos, '\r', avail))
            : nullptr;

        if (cr && cr + 1 < Win() + m_bufLen)
        {
            // A bare CR does not end a line. Waiting for more data would spin until the
            // peer disconnected, so the request line is simply malformed.
            //
            if (cr[1] != '\n') return nullptr;

            if (!ParseRequestLine()) return nullptr;
            m_requestLineEpoch = m_bufEpoch;
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
    char* base = Win();
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

    if (Win()[i] == '?')
    {
        // Capture the raw query for target reconstruction. The caller (GetRequestLine)
        // guarantees the full request line is buffered, so the terminating space/CR is
        // present. Args parsing still consumes from just past the '?' as before.
        //
        size_t queryStart = i + 1;
        size_t j = queryStart;
        while (j < m_bufLen && base[j] != ' ' && base[j] != '\r') j++;

        m_requestLine.query = std::string_view(base + queryStart, j - queryStart);
        m_requestLine.target = std::string_view(base + pathStart, j - pathStart);
        m_parsePos = queryStart;
    }
    else
    {
        m_requestLine.query = {};
        m_requestLine.target = m_requestLine.path;
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
        Phase before = m_phase;
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

        // A skip step that made no progress hit EOF/error mid-phase (e.g. the peer
        // closed while the header block was incomplete). Without this, the phase never
        // advances and the loop spins on recv-returns-EOF. Treat it as terminal.
        //
        if (m_phase == before)
        {
            m_phase = DONE;
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
            ? static_cast<char*>(memchr(Win() + m_parsePos, '\n', avail))
            : nullptr;

        if (nl)
        {
            m_parsePos = (nl - Win()) + 1;
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

    if (m_parsePos >= m_bufLen || Win()[m_parsePos] == ' ' || Win()[m_parsePos] == '\r')
    {
        SkipToHeaders();
        return nullptr;
    }

    size_t nameStart = m_parsePos;

    while (true)
    {
        for (size_t i = (nameStart == m_parsePos ? m_parsePos : m_parsePos); i < m_bufLen; i++)
        {
            char c = Win()[i];
            if (c == '=' || c == '&' || c == ' ' || c == '\r')
            {
                Win()[i] = '\0';
                const char* name = Win() +nameStart;
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
        char c = Win()[i];
        if (c == '&' || c == ' ' || c == '\r')
        {
            m_chunk.data = Win() +valueStart;
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
        m_chunk.data = Win() +valueStart;
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
            char c = Win()[i];
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
    if (m_parseError != 0) return nullptr;

    while (true)
    {
        while (m_bufLen - m_parsePos < 2)
        {
            Compact();
            if (RecvMore() <= 0)
            {
                // The header block never ended. Nothing buffered belongs to a framed
                // request, so the connection does not go on to read it as one.
                //
                m_phase = DONE;
                m_clientClose = true;
                return nullptr;
            }
        }

        if (Win()[m_parsePos] == '\r')
        {
            if (Win()[m_parsePos + 1] != '\n')
            {
                FailRequest(400);
                return nullptr;
            }
            m_parsePos += 2;
            if (!FinishHeaders()) return nullptr;
            m_phase = BODY;
            return nullptr;
        }

        if (++m_headerCount > MAX_HEADER_COUNT)
        {
            FailRequest(431);
            return nullptr;
        }

        size_t nameStart = m_parsePos;

        while (true)
        {
            size_t avail = m_bufLen - m_parsePos;
            char* colon = avail > 0
                ? static_cast<char*>(memchr(Win() + m_parsePos, ':', avail))
                : nullptr;

            if (colon)
            {
                // Check for \r before the colon (malformed)
                //
                char* cr = static_cast<char*>(
                    memchr(Win() + m_parsePos, '\r',
                           colon - (Win() + m_parsePos)));
                if (cr)
                {
                    FailRequest(400);
                    return nullptr;
                }

                size_t i = colon - Win();
                size_t nameEnd = i;
                Win()[i] = '\0';
                const char* name = Win() + nameStart;
                m_parsePos = i + 1;

                // Charge the name plus the ": " and CRLF the line must carry; the value
                // is charged as it streams.
                //
                if (!ChargeHeaderBytes((nameEnd - nameStart) + 4)) return nullptr;

                while (m_parsePos < m_bufLen && Win()[m_parsePos] == ' ')
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

                m_specialValueLen = 0;
                m_valueConsumed = false;
                return name;
            }

            // No colon — check for \r
            //
            char* cr = avail > 0
                ? static_cast<char*>(memchr(Win() + m_parsePos, '\r', avail))
                : nullptr;
            if (cr)
            {
                FailRequest(400);
                return nullptr;
            }

            Compact();
            nameStart = m_parsePos;
            if (RecvMore() <= 0)
            {
                // Either the peer stopped mid-name or the name filled the whole recv
                // buffer without a colon. Neither is a header this parser will produce.
                //
                m_phase = DONE;
                m_clientClose = true;
                return nullptr;
            }
        }
    }
}

template<typename Derived>
Chunk* ConnectionImpl<Derived>::ReadHeaderValue()
{
    if (m_valueConsumed || m_parseError != 0) return nullptr;

    bool special = m_pendingContentLength || m_pendingTransferEncoding
                || m_pendingConnection;

    while (true)
    {
        size_t avail = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
        char* cr = avail > 0
            ? static_cast<char*>(memchr(Win() + m_parsePos, '\r', avail))
            : nullptr;

        if (cr)
        {
            size_t i = static_cast<size_t>(cr - Win());

            // A line terminator is CRLF, and a delimiter that has not fully arrived is
            // not a delimiter. Accepting a lone trailing CR leaves the parser sitting on
            // it, so the next header scan reads that CR and the LF behind it as the blank
            // line ending the block — every header after this one, Content-Length and
            // Transfer-Encoding included, then becomes the head of a second request.
            //
            if (i + 1 >= m_bufLen)
            {
                Compact();
                if (RecvMore() <= 0)
                {
                    FailRequest(400);
                    return nullptr;
                }
                continue;
            }

            if (Win()[i + 1] != '\n')
            {
                FailRequest(400);
                return nullptr;
            }

            m_chunk.data = Win() + m_parsePos;
            m_chunk.size = i - m_parsePos;
            m_chunk.complete = true;

            if (!ChargeHeaderBytes(m_chunk.size)) return nullptr;
            if (special)
            {
                CaptureSpecialValue(static_cast<const char*>(m_chunk.data), m_chunk.size);
            }

            m_parsePos = i + 2;
            m_valueConsumed = true;

            if (special && !ApplySpecialValue()) return nullptr;
            return &m_chunk;
        }

        if (avail > 0)
        {
            // Hand back what is buffered and refill on the next call. Refilling here
            // would compact the window out from under the chunk being returned — the
            // caller would read bytes the next recv had already overwritten.
            //
            m_chunk.data = Win() + m_parsePos;
            m_chunk.size = avail;
            m_chunk.complete = false;

            if (!ChargeHeaderBytes(avail)) return nullptr;
            if (special)
            {
                CaptureSpecialValue(static_cast<const char*>(m_chunk.data), avail);
            }

            m_parsePos = m_bufLen;
            return &m_chunk;
        }

        Compact();
        if (RecvMore() <= 0)
        {
            m_pendingContentLength = false;
            m_pendingTransferEncoding = false;
            m_pendingConnection = false;
            m_specialValueLen = 0;
            m_valueConsumed = true;
            m_phase = DONE;
            m_clientClose = true;
            return nullptr;
        }
    }
}

template<typename Derived>
void ConnectionImpl<Derived>::SkipHeaderValue()
{
    if (m_valueConsumed) return;

    // Skipping is reading and discarding: the same CRLF discipline, the same budget, and
    // the same capture of the values that decide the body's framing.
    //
    while (true)
    {
        Chunk* c = ReadHeaderValue();
        if (!c || c->complete) break;
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
    m_chunk.data = Win() +m_parsePos;
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

// Write the full span to the file, riding out short writes. All writes go through the ring.
//
static bool WriteAllToFile(io::Descriptor& file, const void* data, size_t size, off_t offset)
{
    const char* p = static_cast<const char*>(data);
    size_t remaining = size;
    while (remaining > 0)
    {
        int n = io::Write(file, p, remaining, static_cast<uint64_t>(offset));
        if (n <= 0) return false;
        p += n;
        offset += n;
        remaining -= n;
    }
    return true;
}

template<typename Derived>
int64_t ConnectionImpl<Derived>::ReadBodyToFile(int fileFd, off_t offset)
{
    if (m_phase < BODY)
    {
        if (!AdvanceToPhase(BODY)) return -EPROTO;
    }
    if (m_phase != BODY) return 0;

    int64_t total = 0;
    io::Descriptor file(io::borrowed, fileFd, m_desc.m_ring);

    // Chunked framing interleaves size lines with payload, so chunks bounce through the
    // parser buffer. (Splicing the known-size payload runs is a future refinement.)
    //
    if (m_chunkedBody)
    {
        while (auto* chunk = ReadBody())
        {
            if (!WriteAllToFile(file, chunk->data, chunk->size, offset + total)) return -EIO;
            total += chunk->size;
        }
        return m_chunkedDone ? total : -ECONNRESET;
    }

    if (m_contentLength <= 0)
    {
        m_phase = DONE;
        return 0;
    }
    if (m_bodyRemaining == 0)
    {
        m_bodyRemaining = static_cast<size_t>(m_contentLength);
    }

    // Body bytes already pulled into the recv buffer during header parsing go out first
    //
    size_t buffered = std::min(m_bufLen - m_parsePos, m_bodyRemaining);
    if (buffered > 0)
    {
        if (!WriteAllToFile(file, Win() + m_parsePos, buffered, offset + total)) return -EIO;
        m_parsePos += buffered;
        m_bodyRemaining -= buffered;
        total += buffered;
    }

    if (m_bodyRemaining > 0)
    {
        // Pbuf mode excludes splice: the armed multishot recv continuously drains the
        // socket into pool chunks, so the body bytes are never in the socket for splice
        // to move — they flow through the window bounce instead.
        //
        if (Derived::kSpliceable && !this->m_source)
        {
            // The framed remainder moves socket -> pipe -> page cache without visiting
            // userspace. The splice length is bounded by the bytes remaining, so pipelined
            // data beyond the body stays in the socket for the next request.
            //
            io::PipeLease pipe(m_desc.m_ring->GetPipePool());
            if (!pipe) return -EMFILE;

            // Fairness: a sender that always has bytes ready never blocks us
            // through Poll — the budget forces scheduler passes.
            //
            io::detail::YieldBudget budget(m_ctx);

            while (m_bodyRemaining > 0)
            {
                int n = io::SpliceToFile(m_desc, fileFd, offset + total, pipe.Fds(),
                                         m_bodyRemaining);
                if (n <= 0)
                {
                    pipe.MarkDirty();
                    m_phase = DONE;
                    return n == 0 ? -ECONNRESET : n;
                }
                total += n;
                m_bodyRemaining -= n;
                if (m_bodyRemaining > 0)
                {
                    budget.Charge(static_cast<size_t>(n));
                }
            }
        }
        else
        {
            while (auto* chunk = ReadBody())
            {
                if (!WriteAllToFile(file, chunk->data, chunk->size, offset + total))
                {
                    return -EIO;
                }
                total += chunk->size;
            }
            if (m_bodyRemaining > 0) return -ECONNRESET;
        }
    }

    m_phase = DONE;
    return total;
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
    if (m_chunkedDone || m_parseError != 0) return nullptr;

    if (m_bodyRemaining > 0)
    {
        size_t available = m_bufLen - m_parsePos;
        if (available == 0)
        {
            Compact();
            if (RecvMore() <= 0)
            {
                // The chunk's declared bytes never arrived. The body is short of what it
                // claimed, so it is not complete and the connection cannot be reused.
                //
                m_phase = DONE;
                m_clientClose = true;
                return nullptr;
            }
            available = m_bufLen - m_parsePos;
        }

        size_t toDeliver = std::min(available, m_bodyRemaining);
        m_chunk.data = Win() + m_parsePos;
        m_chunk.size = toDeliver;
        m_bodyRemaining -= toDeliver;
        m_parsePos += toDeliver;
        m_chunk.complete = (m_bodyRemaining == 0);

        if (m_bodyRemaining == 0)
        {
            // Each chunk's data is followed by its own CRLF. Skipping two bytes without
            // reading them lets a peer put anything there, and the two bytes it chooses
            // are then parsed as the start of the next chunk-size line.
            //
            while (m_bufLen - m_parsePos < 2)
            {
                Compact();
                if (RecvMore() <= 0)
                {
                    m_phase = DONE;
                    m_clientClose = true;
                    return &m_chunk;
                }
            }
            if (Win()[m_parsePos] != '\r' || Win()[m_parsePos + 1] != '\n')
            {
                FailRequest(400);
                return nullptr;
            }
            m_parsePos += 2;
        }

        return &m_chunk;
    }

    while (true)
    {
        size_t avail = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
        char* cr = avail > 0
            ? static_cast<char*>(memchr(Win() + m_parsePos, '\r', avail))
            : nullptr;

        if (cr && cr + 1 < Win() + m_bufLen)
        {
            if (cr[1] != '\n')
            {
                FailRequest(400);
                return nullptr;
            }

            // chunk-size is 1*HEXDIG, optionally followed by ';' and extensions. The
            // digits are decoded as hex and nothing else: `c & 0xF` accepts every byte
            // and turns 'a'-'f' into 1-6, so a size line can name a length the sender
            // never wrote, and an unbounded one wraps size_t — to zero, which reads as
            // the terminal chunk and hands the handler a truncated body as a complete
            // one, with the rest of the request left to be parsed as the next.
            //
            size_t i = static_cast<size_t>(cr - Win());
            size_t chunkSize = 0;
            size_t digits = 0;

            for (size_t j = m_parsePos; j < i; j++)
            {
                char c = Win()[j];
                if (c == ';') break;   // chunk extensions, ignored

                int v = HexValue(c);
                if (v < 0 || ++digits > 16)
                {
                    FailRequest(400);
                    return nullptr;
                }
                chunkSize = (chunkSize << 4) | static_cast<size_t>(v);
            }

            if (digits == 0)
            {
                FailRequest(400);
                return nullptr;
            }
            if (chunkSize > MAX_CHUNK_SIZE)
            {
                FailRequest(413);
                return nullptr;
            }

            m_parsePos = i + 2;

            if (chunkSize == 0)
            {
                // The terminal chunk is followed by the trailer section and its blank
                // line. Left buffered, those bytes become the head of the next
                // keep-alive request — and the peer chooses what that request says.
                //
                if (!ConsumeChunkTrailers())
                {
                    m_phase = DONE;
                    return nullptr;
                }
                m_chunkedDone = true;
                m_phase = DONE;
                return nullptr;
            }

            m_bodyRemaining = chunkSize;
            return ReadChunkedBody();
        }

        Compact();
        if (RecvMore() <= 0)
        {
            m_phase = DONE;
            m_clientClose = true;
            return nullptr;
        }
    }
}

// Consume trailer field lines through the blank line that ends the chunked body. Trailer
// lines are charged against the same budget as headers: they are headers, sent late.
//
template<typename Derived>
bool ConnectionImpl<Derived>::ConsumeChunkTrailers()
{
    while (true)
    {
        while (m_bufLen - m_parsePos < 2)
        {
            Compact();
            if (RecvMore() <= 0)
            {
                // The trailer section never ended: the message is unterminated, so the
                // connection is not reused even though the payload arrived.
                //
                m_clientClose = true;
                return false;
            }
        }

        if (Win()[m_parsePos] == '\r')
        {
            if (Win()[m_parsePos + 1] != '\n')
            {
                return FailRequest(400);
            }
            m_parsePos += 2;
            return true;
        }

        // A trailer field line: scan to its CRLF and discard it.
        //
        while (true)
        {
            size_t avail = m_bufLen > m_parsePos ? m_bufLen - m_parsePos : 0;
            char* cr = avail > 0
                ? static_cast<char*>(memchr(Win() + m_parsePos, '\r', avail))
                : nullptr;

            if (cr)
            {
                size_t i = static_cast<size_t>(cr - Win());
                if (i + 1 >= m_bufLen)
                {
                    Compact();
                    if (RecvMore() <= 0)
                    {
                        m_clientClose = true;
                        return false;
                    }
                    continue;
                }
                if (Win()[i + 1] != '\n')
                {
                    return FailRequest(400);
                }
                if (!ChargeHeaderBytes((i - m_parsePos) + 2)) return false;
                m_parsePos = i + 2;
                break;
            }

            Compact();
            if (RecvMore() <= 0)
            {
                m_clientClose = true;
                return false;
            }
        }
    }
}

// -------------------------------------------------------------------------------------
// Write buffer
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::Append(const void* data, size_t size)
{
    if (m_sendError || ResponseClosed()) return false;
    if (size == 0) return true;

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

// Status line for any code: pre-compiled fragment when the code is in the table and no
// custom reason is given, runtime-formatted otherwise.
//
template<typename Derived>
bool ConnectionImpl<Derived>::AppendStatusLine(int status, std::string_view reason)
{
    assert(status >= 100 && status <= 999);

    // Every response path emits its status line here, so this is where a response starts
    // existing as far as a later framing failure is concerned.
    //
    m_responseBegun = true;

    if (reason.empty())
    {
        auto sl = response::StatusLine(status);
        if (sl.data) return Append(sl.data, sl.size);
        reason = response::DefaultReason(status);
    }

    if (!AppendLiteral("HTTP/1.1 ")) return false;
    if (!AppendUInt(static_cast<size_t>(status))) return false;
    if (!AppendLiteral(" ")) return false;
    if (!Append(reason.data(), reason.size())) return false;
    return AppendLiteral(response::CRLF);
}

// -------------------------------------------------------------------------------------
// Response
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::SendRaw(const void* data, size_t size)
{
    assert(!m_sendError);

    if (ResponseClosed()) return false;

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

    if (!AppendStatusLine(status, {})) return false;
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

    if (!AppendStatusLine(status, {})) return false;
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

// Chunked response headers, deferred from BeginChunked until the first chunk (or EndChunked)
// so the status line and headers coalesce with the first data flush.
//
template<typename Derived>
bool ConnectionImpl<Derived>::AppendChunkedHeaders()
{
    m_chunkedHeadersPending = false;

    if (!AppendStatusLine(m_chunkedStatus, {})) return false;
    if (!AppendLiteral(response::CONTENT_TYPE)) return false;
    if (!Append(m_chunkedContentType, strlen(m_chunkedContentType))) return false;
    if (!AppendLiteral(response::CRLF)) return false;
    if (!AppendLiteral(response::TRANSFER_ENCODING_CHUNKED)) return false;
    return AppendConnectionTrailer();
}

template<typename Derived>
bool ConnectionImpl<Derived>::SendChunk(const void* data, size_t size)
{
    assert(!m_sendError);
    if (size == 0) return false;

    if (m_chunkedHeadersPending && !AppendChunkedHeaders())
    {
        return false;
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

    if (m_chunkedHeadersPending && !AppendChunkedHeaders())
    {
        return false;
    }

    if (!AppendLiteral(response::CHUNKED_TERMINATOR)) return false;
    return Flush();
}

template<typename Derived>
bool ConnectionImpl<Derived>::EndChunked(const void* lastChunkData, size_t lastChunkSize)
{
    assert(!m_sendError);
    if (lastChunkSize == 0) return EndChunked();

    if (m_chunkedHeadersPending && !AppendChunkedHeaders())
    {
        return false;
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

    if (ResponseClosed()) return false;

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
// Component response API — status line / headers / body as separate steps, for proxying
// and responses the composed methods don't cover
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::BeginResponse(int status, std::string_view reason)
{
    assert(!m_sendError);
    return AppendStatusLine(status, reason);
}

template<typename Derived>
bool ConnectionImpl<Derived>::AppendHeader(const char* name, std::string_view value)
{
    if (!Append(name, strlen(name))) return false;
    if (!AppendLiteral(": ")) return false;
    if (!Append(value.data(), value.size())) return false;
    return AppendLiteral(response::CRLF);
}

template<typename Derived>
bool ConnectionImpl<Derived>::AppendHeader(const char* name, size_t value)
{
    if (!Append(name, strlen(name))) return false;
    if (!AppendLiteral(": ")) return false;
    if (!AppendUInt(value)) return false;
    return AppendLiteral(response::CRLF);
}

template<typename Derived>
bool ConnectionImpl<Derived>::EndHeaders()
{
    if (!AppendConnectionTrailer()) return false;
    return Flush();
}

// -------------------------------------------------------------------------------------
// SendRawBytes — for protocol upgrade (101 Switching Protocols)
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::SendRawBytes(const void* data, size_t size)
{
    if (!Flush()) return false;
    return SendRaw(data, size);
}

// -------------------------------------------------------------------------------------
// Explicit template instantiations for known transport types
// -------------------------------------------------------------------------------------

template struct ConnectionImpl<Connection<PlaintextTransport>>;
template struct ConnectionImpl<Connection<TlsTransport>>;

} // end namespace coop::http
} // end namespace coop
