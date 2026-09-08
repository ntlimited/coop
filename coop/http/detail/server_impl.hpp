#pragma once

#include "coop/http/connection.h"
#include "coop/http/response_constants.h"

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
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

namespace detail
{
inline constexpr auto kServerTokens = []
{
    std::array<bool, 256> tokens{};
    for (unsigned c = 'a'; c <= 'z'; ++c) tokens[c] = true;
    for (unsigned c = 'A'; c <= 'Z'; ++c) tokens[c] = true;
    for (unsigned c = '0'; c <= '9'; ++c) tokens[c] = true;
    for (unsigned char c : "!#$%&'*+-.^_`|~") if (c) tokens[c] = true;
    return tokens;
}();
inline bool ServerToken(unsigned char c) { return kServerTokens[c]; }
inline bool ServerValueChar(unsigned char c)
{
    return c == '\t' || (c >= 0x20 && c != 0x7f);
}
inline unsigned char ServerLower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
} // namespace detail

template<typename Derived>
ConnectionImpl<Derived>::ConnectionImpl(io::Descriptor& desc, Context* ctx, Cooperator* co,
                                        time::Interval timeout, ServerParserOptions options)
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
, m_options(options)
, m_error(0)
, m_fieldNumber(0)
, m_fieldState(0)
, m_tokenLength(0)
, m_tokenMatches(7)
, m_lastTokenChunked(false)
, m_valueStarted(false)
, m_needChunkCrlf(false)
{
}

template<typename Derived>
bool ConnectionImpl<Derived>::Reset()
{
    // A failed request is not a request boundary: the bytes still buffered belong to a
    // message this parser could not frame, so re-reading them as the next request is the
    // step that turns a framing bug into a smuggled request. Reset is a no-op instead;
    // the connection is already close-framed and its loop exits.
    //
    if (!Reusable()) return false;

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
    m_fieldNumber = 0;
    m_fieldState = 0;
    m_tokenLength = 0;
    m_tokenMatches = 7;
    m_lastTokenChunked = false;
    m_valueStarted = false;
    m_needChunkCrlf = false;
    m_keepAlive = true;
    return true;
}

template<typename Derived>
bool ConnectionImpl<Derived>::Fail(int error)
{
    if (m_error) return false;
    if (error == -EPROTO || error == -EOVERFLOW || error == -EMSGSIZE)
        FailRequest(error == -EMSGSIZE ? 431 : 400);
    m_error = error;
    m_phase = DONE;
    m_clientClose = true;
    return false;
}

template<typename Derived>
bool ConnectionImpl<Derived>::FailIo(int error)
{
    if (!m_error) m_error = error;
    m_phase = DONE;
    m_clientClose = true;
    return false;
}

template<typename Derived>
int ConnectionImpl<Derived>::RecvMore()
{
    if (m_error) return m_error;
    int n = detail::ParserBuffer<Derived>::RecvMore();
    if (n == -EMSGSIZE) Fail(n);
    else if (n <= 0) FailIo(n == 0 ? -ECONNRESET : n);
    return n;
}

template<typename Derived>
bool ConnectionImpl<Derived>::Receive()
{
    Compact();
    return RecvMore() > 0;
}

template<typename Derived>
bool ConnectionImpl<Derived>::Ensure(size_t size)
{
    while (m_bufLen - m_parsePos < size) if (!Receive()) return false;
    return !m_error;
}

template<typename Derived>
bool ConnectionImpl<Derived>::ConsumeCrlf()
{
    if (!Ensure(2)) return false;
    if (Win()[m_parsePos] != '\r' || Win()[m_parsePos + 1] != '\n') return Fail(-EPROTO);
    m_parsePos += 2;
    return true;
}

// -------------------------------------------------------------------------------------
// Buffer management
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ConnectionImpl<Derived>::RecvAborted() const
{
    return m_ctx && m_ctx->IsKilled();
}

// -------------------------------------------------------------------------------------
// Framing enforcement
// -------------------------------------------------------------------------------------

namespace detail
{

// Reason phrase and body for the statuses a framing failure can answer with. 431 is not
// in the pre-compiled status table, so its phrase is supplied here rather than falling
// back to the status-class default.
//
struct ServerFailureText
{
    const char* reason;
    const char* body;
};

inline ServerFailureText ServerTextForFailure(int status)
{
    switch (status)
    {
        case 413: return { "Payload Too Large", "Payload Too Large\n" };
        case 431: return { "Request Header Fields Too Large",
                           "Request Header Fields Too Large\n" };
        default:  return { "Bad Request", "Bad Request\n" };
    }
}

} // namespace detail

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
    if (m_options.errorResponse == ParserErrorResponse::Automatic &&
        !m_responseBegun && !m_sendError)
    {
        detail::ServerFailureText text = detail::ServerTextForFailure(status);
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
    if (!m_error) m_error = -EPROTO;
    return false;
}

// Header and trailer lines are charged against one cumulative budget. The recv buffer
// bounds a single line, but the parser compacts and refills as it scans, so without an
// explicit budget a peer can stream header bytes for as long as it likes.
//
template<typename Derived>
bool ConnectionImpl<Derived>::ChargeHeaderBytes(size_t bytes)
{
    if (bytes > m_options.maxHeaderBytes - m_headerBytes)
    {
        return FailRequest(431);
    }
    m_headerBytes += bytes;
    return true;
}

// Framing metadata is recognized incrementally in the very spans exposed to callers.
// Token matching retains only a mask and a saturating length, never an owning copy or
// a header-sized scratch buffer. Arbitrarily long unknown connection/transfer tokens
// therefore cost constant state.
//
template<typename Derived>
bool ConnectionImpl<Derived>::FinishHeaderToken()
{
    if (!m_tokenLength) return Fail(-EPROTO);
    bool chunked = (m_tokenMatches & 1) && m_tokenLength == 7;
    if (m_pendingTransferEncoding)
    {
        if (m_chunkedBody) return Fail(-EPROTO); // chunked must occur once, last
        m_chunkedBody = chunked;
        m_haveTransferEncoding = true;
        m_lastTokenChunked = chunked;
    }
    else
    {
        if ((m_tokenMatches & 2) && m_tokenLength == 5) m_clientClose = true;
        if ((m_tokenMatches & 4) && m_tokenLength == 10) m_keepAlive = true;
    }
    m_tokenLength = 0;
    m_tokenMatches = 7;
    return true;
}

template<typename Derived>
bool ConnectionImpl<Derived>::FeedHeaderValue(const char* data, size_t size, bool final)
{
    // Exact single-span common values need no token DFA. The equality itself checks
    // the whole grammar; fragmented values, lists and parameters use the same
    // incremental recognizer below. Trailing OWS is part of field syntax, not a token.
    //
    if (final && m_fieldState == 0 && (m_pendingConnection || m_pendingTransferEncoding))
    {
        size_t length = size;
        while (length && (data[length - 1] == ' ' || data[length - 1] == '\t')) --length;
        if (m_pendingConnection)
        {
            if (length == 10 && !strncasecmp(data, "keep-alive", 10))
            {
                m_keepAlive = true;
                return true;
            }
            if (length == 5 && !strncasecmp(data, "close", 5))
            {
                m_clientClose = true;
                return true;
            }
        }
        else if (length == 7 && !strncasecmp(data, "chunked", 7))
        {
            if (m_chunkedBody) return Fail(-EPROTO);
            m_haveTransferEncoding = true;
            m_chunkedBody = true;
            return true;
        }
    }
    if (m_pendingContentLength)
    {
        for (size_t i = 0; i < size; ++i)
        {
            const char c = data[i];
            if (c >= '0' && c <= '9')
            {
                if (m_fieldState == 2) return Fail(-EPROTO);
                const uint64_t digit = c - '0';
                if (m_fieldNumber > (uint64_t(INT64_MAX) - digit) / 10)
                    return Fail(-EOVERFLOW);
                m_fieldNumber = m_fieldNumber * 10 + digit;
                m_fieldState = 1;
            }
            else if (c == ' ' || c == '\t')
            {
                if (m_fieldState == 1) m_fieldState = 2;
            }
            else if (c == ',' && m_fieldState)
            {
                if (m_contentLength >= 0 && uint64_t(m_contentLength) != m_fieldNumber)
                    return Fail(-EPROTO);
                m_contentLength = static_cast<int64_t>(m_fieldNumber);
                m_fieldNumber = 0;
                m_fieldState = 0;
            }
            else return Fail(-EPROTO);
        }
        if (final)
        {
            if (!m_fieldState) return Fail(-EPROTO);
            if (m_contentLength >= 0 && uint64_t(m_contentLength) != m_fieldNumber)
                return Fail(-EPROTO);
            m_contentLength = static_cast<int64_t>(m_fieldNumber);
        }
        return true;
    }
    if (!m_pendingTransferEncoding && !m_pendingConnection) return true;

    // 0 token start, 1 token, 2 after token; 3 parameter name start, 4 name,
    // 5 equals, 6 value start, 7 token value, 8 after parameter, 9 quoted, 10 escape.
    //
    for (size_t i = 0; i < size; ++i)
    {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (m_fieldState == 9)
        {
            if (c == '"') m_fieldState = 8;
            else if (c == '\\') m_fieldState = 10;
            continue;
        }
        if (m_fieldState == 10) { m_fieldState = 9; continue; }
        const bool ows = c == ' ' || c == '\t';
        if (m_fieldState == 0 || m_fieldState == 1)
        {
            if (detail::ServerToken(c))
            {
                static constexpr const char* tokens[] = {"chunked", "close", "keep-alive"};
                for (unsigned t = 0; t != 3; ++t)
                {
                    if ((m_tokenMatches & (1u << t)) &&
                        detail::ServerLower(c) != static_cast<unsigned char>(tokens[t][m_tokenLength]))
                        m_tokenMatches &= ~(1u << t);
                }
                if (m_tokenLength < 11) ++m_tokenLength;
                m_fieldState = 1;
                continue;
            }
            if (m_fieldState == 0)
            {
                if (ows || c == ',') continue; // recipients ignore empty list members
                return Fail(-EPROTO);
            }
            if (!FinishHeaderToken()) return false;
            m_fieldState = 2;
        }
        if (m_fieldState == 2 || m_fieldState == 8)
        {
            if (ows) continue;
            if (c == ',') { m_fieldState = 0; continue; }
            if (c == ';' && m_pendingTransferEncoding && !m_lastTokenChunked)
            {
                m_fieldState = 3;
                continue;
            }
            return Fail(-EPROTO);
        }
        if (m_fieldState == 3)
        {
            if (ows) continue;
            if (!detail::ServerToken(c)) return Fail(-EPROTO);
            m_fieldState = 4;
        }
        else if (m_fieldState == 4)
        {
            if (detail::ServerToken(c)) continue;
            if (ows) { m_fieldState = 5; continue; }
            if (c != '=') return Fail(-EPROTO);
            m_fieldState = 6;
        }
        else if (m_fieldState == 5)
        {
            if (ows) continue;
            if (c != '=') return Fail(-EPROTO);
            m_fieldState = 6;
        }
        else if (m_fieldState == 6)
        {
            if (ows) continue;
            if (c == '"') { m_fieldState = 9; continue; }
            if (!detail::ServerToken(c)) return Fail(-EPROTO);
            m_fieldState = 7;
        }
        else if (m_fieldState == 7)
        {
            if (detail::ServerToken(c)) continue;
            m_fieldState = 8;
            if (ows) continue;
            if (c == ';') { m_fieldState = 3; continue; }
            if (c == ',') { m_fieldState = 0; continue; }
            return Fail(-EPROTO);
        }
    }
    if (final)
    {
        if (m_fieldState == 1)
        {
            if (!FinishHeaderToken()) return false;
            m_fieldState = 2;
        }
        if (m_fieldState != 0 && m_fieldState != 2 && m_fieldState != 7 && m_fieldState != 8)
            return Fail(-EPROTO);
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
    if (m_contentLength >= 0 && m_haveTransferEncoding)
    {
        return FailRequest(400);
    }
    if (m_haveTransferEncoding && !m_chunkedBody) return Fail(-EPROTO);
    m_haveContentLength = m_contentLength >= 0;
    m_phase = BODY;
    if (!m_chunkedBody)
    {
        m_bodyRemaining = m_contentLength > 0 ? static_cast<size_t>(m_contentLength) : 0;
        if (!m_bodyRemaining) m_phase = DONE;
    }
    return true;
}

// -------------------------------------------------------------------------------------
// Phase 1: Request line
// -------------------------------------------------------------------------------------

template<typename Derived>
RequestLine* ConnectionImpl<Derived>::GetRequestLine()
{
    if (m_error) return nullptr;
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
            assert(false && "RequestLine views invalidated by parsing — "
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
            if (cr[1] != '\n') { Fail(-EPROTO); return nullptr; }

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
    char* p = base + m_parsePos;
    char* end = static_cast<char*>(memchr(p, '\r', m_bufLen - m_parsePos));
    if (!end) return Fail(-EPROTO);

    // Find method end (first space)
    //
    char* sp = static_cast<char*>(memchr(p, ' ', end - p));
    if (!sp) return Fail(-EPROTO);

    if (sp == p) return Fail(-EPROTO);
    for (char* q = p; q != sp; ++q)
        if (!detail::ServerToken(static_cast<unsigned char>(*q))) return Fail(-EPROTO);
    m_requestLine.method = std::string_view(p, sp - p);
    p = sp + 1;
    size_t pathStart = p - base;

    // Path end — multi-delimiter, keep byte loop (typically < 20 chars)
    //
    size_t i = pathStart;
    while (i < m_bufLen && base[i] != '?' && base[i] != ' ' && base[i] != '\r') i++;
    if (i >= m_bufLen) return Fail(-EPROTO);

    char* targetEnd = static_cast<char*>(memchr(base + pathStart, ' ', end - base - pathStart));
    if (!targetEnd || targetEnd == base + pathStart) return Fail(-EPROTO);
    char* lineEnd = end;
    std::string_view version(targetEnd + 1, lineEnd - targetEnd - 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return Fail(-EPROTO);
    m_requestLine.version = version;
    m_keepAlive = version == "HTTP/1.1";
    for (char* q = base + pathStart; q != targetEnd; ++q)
        if (static_cast<unsigned char>(*q) <= 0x20 || *q == 0x7f) return Fail(-EPROTO);
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
                m_requestLineEpoch = m_bufEpoch - 1; // raw target/query now change in place
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
                    else SkipToHeaders();
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
    if (m_phase < HEADERS && !AdvanceToPhase(HEADERS)) return nullptr;
    if (m_phase != HEADERS || m_error) return nullptr;
    if (!m_valueConsumed) SkipHeaderValue();
    if (m_error) return nullptr;
    if (!Ensure(2)) return nullptr;
    if (Win()[m_parsePos] == '\r')
    {
        if (!ConsumeCrlf()) return nullptr;
        FinishHeaders();
        return nullptr;
    }
    if (m_headerCount == m_options.maxHeaderCount) { FailRequest(431); return nullptr; }
    ++m_headerCount;
    while (true)
    {
        char* start = Win() + m_parsePos;
        size_t available = m_bufLen - m_parsePos;
        char* colon = static_cast<char*>(memchr(start, ':', available));
        if (colon)
        {
            const size_t length = colon - start;
            m_pendingContentLength = length == 14 && !strncasecmp(start, "content-length", 14);
            m_pendingTransferEncoding = length == 17 && !strncasecmp(start, "transfer-encoding", 17);
            m_pendingConnection = length == 10 && !strncasecmp(start, "connection", 10);
            // A recognized name is already known to consist of token characters.
            // Other names retain full grammar validation without a second name scan
            // on the framing-only common path.
            //
            if (!m_pendingContentLength && !m_pendingTransferEncoding && !m_pendingConnection)
            {
                if (!length) { Fail(-EPROTO); return nullptr; }
                for (size_t i = 0; i < length; ++i)
                {
                    if (!detail::ServerToken(static_cast<unsigned char>(start[i])))
                    {
                        Fail(-EPROTO);
                        return nullptr;
                    }
                }
            }
            if (!ChargeHeaderBytes(length + 3)) return nullptr;
            *colon = '\0';
            m_parsePos += length + 1;
            if (m_pendingContentLength)
            {
                m_fieldNumber = 0;
                m_fieldState = 0;
            }
            else if (m_pendingTransferEncoding || m_pendingConnection)
            {
                m_fieldState = 0;
                m_tokenLength = 0;
                m_tokenMatches = 7;
                m_lastTokenChunked = false;
            }
            m_valueConsumed = false;
            m_valueStarted = false;
            return start;
        }
        for (size_t i = 0; i < available; ++i)
        {
            if (!detail::ServerToken(static_cast<unsigned char>(start[i])))
            {
                Fail(-EPROTO);
                return nullptr;
            }
        }
        if (!Receive()) return nullptr;
    }
}

template<typename Derived>
Chunk* ConnectionImpl<Derived>::ReadHeaderValue()
{
    if (m_valueConsumed || m_error) return nullptr;
    while (true)
    {
        if (m_parsePos == m_bufLen && !Receive()) return nullptr;
        if (!m_valueStarted)
        {
            while (m_parsePos < m_bufLen &&
                   (Win()[m_parsePos] == ' ' || Win()[m_parsePos] == '\t'))
            {
                if (!ChargeHeaderBytes(1)) return nullptr;
                ++m_parsePos;
            }
            if (m_parsePos == m_bufLen) continue;
            m_valueStarted = true;
        }
        size_t start = m_parsePos;
        size_t end = start;
        while (end < m_bufLen && Win()[end] != '\r')
        {
            if (!detail::ServerValueChar(static_cast<unsigned char>(Win()[end])))
            {
                Fail(-EPROTO);
                return nullptr;
            }
            ++end;
        }
        const bool haveCrlf = end + 1 < m_bufLen;
        if (end < m_bufLen && haveCrlf && Win()[end + 1] != '\n')
        {
            Fail(-EPROTO);
            return nullptr;
        }
        if (end == start && !haveCrlf)
        {
            if (!Receive()) return nullptr;
            continue;
        }
        if (!ChargeHeaderBytes(end - start)) return nullptr;
        if (!FeedHeaderValue(Win() + start, end - start, haveCrlf)) return nullptr;
        m_chunk = {Win() + start, end - start, haveCrlf};
        m_parsePos = end;
        if (haveCrlf)
        {
            m_parsePos += 2;
            m_valueConsumed = true;
        }
        // Never refill after forming a view, even to finish a split CRLF. The next
        // pull validates/consumes that delimiter and may return an empty final span.
        //
        return &m_chunk;
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
BodyResult ConnectionImpl<Derived>::NextBody()
{
    Chunk* chunk = ReadBody();
    return {chunk, m_error};
}

template<typename Derived>
Chunk* ConnectionImpl<Derived>::ReadBody()
{
    if (m_error) return nullptr;
    if (m_phase < BODY && !AdvanceToPhase(BODY)) return nullptr;
    if (m_phase != BODY) return nullptr;
    if (m_chunkedBody) return ReadChunkedBody();
    if (!m_bodyRemaining) { m_phase = DONE; return nullptr; }
    if (m_parsePos == m_bufLen && !Receive()) return nullptr;
    size_t size = std::min(m_bufLen - m_parsePos, m_bodyRemaining);
    m_chunk = {Win() + m_parsePos, size, size == m_bodyRemaining};
    m_parsePos += size;
    m_bodyRemaining -= size;
    if (!m_bodyRemaining) m_phase = DONE;
    return &m_chunk;
}

// Disk writes are explicit caller-requested work. Keep their errno instead of masking
// a failed sink as a successful/truncated response.
//
inline int ServerWriteAllToFile(io::Descriptor& file, const void* data, size_t size, off_t offset)
{
    auto* p = static_cast<const char*>(data);
    while (size)
    {
        int n = io::Write(file, p, size, static_cast<uint64_t>(offset));
        if (n <= 0) return n < 0 ? n : -EIO;
        p += n;
        offset += n;
        size -= n;
    }
    return 0;
}

template<typename Derived>
int64_t ConnectionImpl<Derived>::ReadBodyToFile(int fileFd, off_t offset)
{
    if (m_error) return m_error;
    if (offset < 0) return -EINVAL;
    if (m_phase < BODY && !AdvanceToPhase(BODY)) return m_error;
    if (m_phase != BODY) return 0;
    auto& desc = m_desc;
    io::Descriptor file(io::borrowed, fileFd, desc.m_ring);
    int64_t total = 0;

    // Only a fixed-length body permits bounding a kernel splice away from the next
    // response. A recv source already owns the socket stream, so it must be drained
    // through its borrowed windows. TLS and chunked framing use the same pull path.
    //
    if constexpr (Derived::kSpliceable)
    {
        if (!m_chunkedBody && !this->m_source)
        {
            if (m_bodyRemaining > uint64_t(INT64_MAX - offset)) return -EOVERFLOW;
            size_t buffered = std::min(m_bufLen - m_parsePos, m_bodyRemaining);
            if (buffered)
            {
                int error = ServerWriteAllToFile(file, Win() + m_parsePos, buffered, offset);
                if (error) { FailIo(error); return error; }
                m_parsePos += buffered;
                m_bodyRemaining -= buffered;
                total += buffered;
            }
            if (m_bodyRemaining)
            {
                io::PipeLease pipe(desc.m_ring->GetPipePool());
                if (!pipe) { FailIo(-EMFILE); return m_error; }
                io::detail::YieldBudget budget(m_ctx);
                while (m_bodyRemaining)
                {
                    int n = io::SpliceToFile(desc, fileFd, offset + total, pipe.Fds(), m_bodyRemaining);
                    if (n <= 0)
                    {
                        pipe.MarkDirty();
                        FailIo(n == 0 ? -ECONNRESET : n);
                        return m_error;
                    }
                    total += n;
                    m_bodyRemaining -= n;
                    if (m_bodyRemaining) budget.Charge(static_cast<size_t>(n));
                }
            }
            m_phase = DONE;
            return total;
        }
    }
    while (auto* chunk = ReadBody())
    {
        if (chunk->size > uint64_t(INT64_MAX - offset - total))
        {
            FailIo(-EOVERFLOW);
            return m_error;
        }
        int error = ServerWriteAllToFile(file, chunk->data, chunk->size, offset + total);
        if (error) { FailIo(error); return error; }
        total += chunk->size;
    }
    return m_error ? m_error : total;
}
template<typename Derived>
bool ConnectionImpl<Derived>::SkipBody()
{
    while (ReadBody()) {}
    return Complete();
}

// -------------------------------------------------------------------------------------
// Chunked body parsing
// -------------------------------------------------------------------------------------

template<typename Derived>
Chunk* ConnectionImpl<Derived>::ReadChunkedBody()
{
    if (m_needChunkCrlf)
    {
        if (!ConsumeCrlf()) return nullptr;
        m_needChunkCrlf = false;
    }
    if (!m_bodyRemaining)
    {
        size_t size = 0;
        bool digits = false;
        bool extension = false;
        // 0 semicolon, 1 name start, 2 name, 3 after name, 4 value start,
        // 5 token value, 6 after value, 7 quoted value, 8 quoted escape.
        uint8_t extensionState = 0;
        while (true)
        {
            if (!Ensure(1)) return nullptr;
            unsigned char c = static_cast<unsigned char>(Win()[m_parsePos]);
            if (c == '\r')
            {
                if (!digits || (extension && extensionState != 2 && extensionState != 3 &&
                                extensionState != 5 && extensionState != 6) || !ConsumeCrlf())
                {
                    if (!m_error) Fail(-EPROTO);
                    return nullptr;
                }
                break;
            }
            ++m_parsePos;
            if (!extension && digits && (c == ';' || c == ' ' || c == '\t'))
                extension = true;
            if (extension)
            {
                const bool ows = c == ' ' || c == '\t';
                if (!detail::ServerValueChar(c)) { Fail(-EPROTO); return nullptr; }
                if (extensionState == 7)
                {
                    if (c == '"') extensionState = 6;
                    else if (c == '\\') extensionState = 8;
                    continue;
                }
                if (extensionState == 8) { extensionState = 7; continue; }
                if (extensionState == 0 || extensionState == 3 || extensionState == 6)
                {
                    if (ows) continue;
                    if (c == ';') { extensionState = 1; continue; }
                    if (extensionState == 3 && c == '=') { extensionState = 4; continue; }
                    Fail(-EPROTO);
                    return nullptr;
                }
                if (extensionState == 1)
                {
                    if (ows) continue;
                    if (detail::ServerToken(c)) { extensionState = 2; continue; }
                }
                else if (extensionState == 2)
                {
                    if (detail::ServerToken(c)) continue;
                    if (ows) { extensionState = 3; continue; }
                    if (c == '=') { extensionState = 4; continue; }
                    if (c == ';') { extensionState = 1; continue; }
                }
                else if (extensionState == 4)
                {
                    if (ows) continue;
                    if (c == '"') { extensionState = 7; continue; }
                    if (detail::ServerToken(c)) { extensionState = 5; continue; }
                }
                else if (extensionState == 5)
                {
                    if (detail::ServerToken(c)) continue;
                    if (ows) { extensionState = 6; continue; }
                    if (c == ';') { extensionState = 1; continue; }
                }
                Fail(-EPROTO);
                return nullptr;
            }
            int digit = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) { Fail(-EPROTO); return nullptr; }
            if (size > (std::numeric_limits<size_t>::max() - digit) / 16)
            {
                Fail(-EOVERFLOW);
                return nullptr;
            }
            digits = true;
            size = size * 16 + digit;
        }
        if (size > m_options.maxChunkSize) { FailRequest(413); return nullptr; }
        if (!size)
        {
            // Trailers are framing, not response headers. Validate and consume them
            // without storing them or allowing them to alter the selected framing.
            // Each line streams, so the caller's buffer size is not a trailer limit.
            //
            while (true)
            {
                if (!Ensure(1)) return nullptr;
                if (Win()[m_parsePos] == '\r')
                {
                    if (!ConsumeCrlf()) return nullptr;
                    m_chunkedDone = true;
                    m_phase = DONE;
                    return nullptr;
                }
                if (m_headerCount == m_options.maxHeaderCount) { FailRequest(431); return nullptr; }
                ++m_headerCount;
                if (!ChargeHeaderBytes(2)) return nullptr;
                bool name = true;
                bool haveName = false;
                while (true)
                {
                    if (!Ensure(1)) return nullptr;
                    unsigned char c = static_cast<unsigned char>(Win()[m_parsePos]);
                    if (c == '\r')
                    {
                        if (name || !ConsumeCrlf())
                        {
                            if (!m_error) Fail(-EPROTO);
                            return nullptr;
                        }
                        break;
                    }
                    ++m_parsePos;
                    if (!ChargeHeaderBytes(1)) return nullptr;
                    if (name)
                    {
                        if (c == ':' && haveName) name = false;
                        else if (detail::ServerToken(c)) haveName = true;
                        else { Fail(-EPROTO); return nullptr; }
                    }
                    else if (!detail::ServerValueChar(c)) { Fail(-EPROTO); return nullptr; }
                }
            }
        }
        m_bodyRemaining = size;
    }
    if (m_parsePos == m_bufLen && !Receive()) return nullptr;
    size_t size = std::min(m_bufLen - m_parsePos, m_bodyRemaining);
    m_chunk = {Win() + m_parsePos, size, false};
    m_parsePos += size;
    m_bodyRemaining -= size;
    // Chunk::complete continues to mean the end of this wire chunk, not the entire
    // request. NextBody().Complete()/Complete() report message completion after the
    // zero chunk and trailers. The delimiter is consumed only on the next pull.
    //
    m_chunk.complete = m_bodyRemaining == 0;
    m_needChunkCrlf = m_bodyRemaining == 0;
    return &m_chunk;
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
    if (m_sendError || ResponseClosed()) return false;

    auto* p = static_cast<const char*>(data);
    while (size)
    {
        size_t part = std::min(size, static_cast<size_t>(INT_MAX));
        int result = TransportSendAll(p, part);
        if (result <= 0 || static_cast<size_t>(result) != part)
        {
            m_sendError = true;
            FailIo(result < 0 ? result : -EIO);
            return false;
        }
        p += part;
        size -= part;
    }
    return true;
}

// Append the common header block: status line, Content-Type, Content-Length, Connection.
//
template<typename Derived>
bool ConnectionImpl<Derived>::SendHeaders(int status, const char* contentType,
                                           size_t contentLength)
{
    if (m_sendError || ResponseClosed()) return false;

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
    if (m_sendError || ResponseClosed()) return false;

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
    if (m_sendError || ResponseClosed()) return false;

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
    if (m_sendError || ResponseClosed()) return false;
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
    if (m_sendError || ResponseClosed()) return false;

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
    if (m_sendError || ResponseClosed()) return false;
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
    if (m_sendError || ResponseClosed()) return false;
    if (offset < 0) return FailIo(-EINVAL);
    if (count > static_cast<uint64_t>(std::numeric_limits<off_t>::max() - offset))
        return FailIo(-EOVERFLOW);
    if (!Flush()) return false;
    while (count)
    {
        size_t take = std::min(count, static_cast<size_t>(INT_MAX));
        int result = TransportSendfileAll(fileFd, offset, take);
        if (result <= 0 || static_cast<size_t>(result) != take)
        {
            m_sendError = true;
            return FailIo(result < 0 ? result : -EIO);
        }
        count -= take;
        offset += take;
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
    if (m_sendError || ResponseClosed()) return false;
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

} // end namespace coop::http
} // end namespace coop
