#pragma once

// Template implementation is separate from the public interface so custom transports and
// deterministic protocol tests can instantiate the same parser as the native transports.
//
#include "coop/http/client.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <strings.h>

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
inline constexpr auto kClientTokens = []
{
    std::array<bool, 256> tokens{};
    for (unsigned c = 'a'; c <= 'z'; ++c) tokens[c] = true;
    for (unsigned c = 'A'; c <= 'Z'; ++c) tokens[c] = true;
    for (unsigned c = '0'; c <= '9'; ++c) tokens[c] = true;
    for (unsigned char c : "!#$%&'*+-.^_`|~") if (c) tokens[c] = true;
    return tokens;
}();
inline bool ClientToken(unsigned char c) { return kClientTokens[c]; }
inline bool ClientValueChar(unsigned char c)
{
    return c == '\t' || (c >= 0x20 && c != 0x7f);
}
inline unsigned char ClientLower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
} // namespace detail

template<typename Derived>
ClientConnectionImpl<Derived>::ClientConnectionImpl(const char* host, time::Interval timeout)
: m_host(host)
, m_timeout(timeout)
, m_sendLen(0)
, m_isConnect(false)
, m_isHead(false)
{
    ClearResponse();
}

template<typename Derived>
void ClientConnectionImpl<Derived>::ClearResponse()
{
    m_phase = RESPONSE_LINE;
    m_contentLength = -1;
    m_chunkedBody = false;
    m_bodyRemaining = 0;
    m_responseLine = {};
    m_chunk = {};
    m_responseLineEpoch = 0;
    m_responseLineParsed = false;
    m_error = 0;
    m_needChunkCrlf = false;
    m_closeDelimited = false;
    m_transferEncoding = false;
    m_upgraded = false;
    m_valueStarted = false;
    m_fieldNumber = 0;
    m_fieldState = 0;
    m_tokenLength = 0;
    m_tokenMatches = 0;
    m_lastTokenChunked = false;
    m_valueConsumed = true;
    m_pendingContentLength = false;
    m_pendingTransferEncoding = false;
    m_pendingConnection = false;
    m_keepAlive = true;
    m_serverClose = false;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::Reset()
{
    if (!Reusable()) return false;
    Compact();
    ClearResponse();
    m_sendLen = 0;
    m_isHead = false;
    m_isConnect = false;
    return true;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::AdvanceResponse()
{
    if (!Complete() || m_responseLine.status < 100 || m_responseLine.status >= 200 ||
        m_responseLine.status == 101) return false;
    Compact();
    ClearResponse();
    return true;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::Fail(int error)
{
    if (!m_error) m_error = error;
    m_phase = DONE;
    return false;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::Receive(bool eofCompletes)
{
    Compact();
    int n = RecvMore();
    if (n > 0) return true;
    if (n == 0 && eofCompletes)
    {
        m_phase = DONE;
        return false;
    }
    return Fail(n == 0 ? -ECONNRESET : n);
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::Ensure(size_t size)
{
    while (m_bufLen - m_parsePos < size)
    {
        if (!Receive()) return false;
    }
    return true;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::ConsumeCrlf()
{
    if (!Ensure(2)) return false;
    if (Win()[m_parsePos] != '\r' || Win()[m_parsePos + 1] != '\n')
        return Fail(-EPROTO);
    m_parsePos += 2;
    return true;
}

template<typename Derived>
ResponseLine* ClientConnectionImpl<Derived>::GetResponseLine()
{
    if (m_error) return nullptr;
    if (m_responseLineParsed)
    {
        if (m_responseLineEpoch != m_bufEpoch)
        {
            assert(false && "ResponseLine view invalidated by buffer compaction");
            return nullptr;
        }
        return &m_responseLine;
    }
    while (true)
    {
        size_t available = m_bufLen - m_parsePos;
        char* cr = available ? static_cast<char*>(
            memchr(Win() + m_parsePos, '\r', available)) : nullptr;
        if (cr && cr + 1 < Win() + m_bufLen)
        {
            if (cr[1] != '\n' || !ParseResponseLine())
            {
                Fail(-EPROTO);
                return nullptr;
            }
            m_parsePos = cr - Win() + 2;
            m_responseLineEpoch = m_bufEpoch;
            m_responseLineParsed = true;
            m_phase = HEADERS;
            return &m_responseLine;
        }
        if (!Receive()) return nullptr;
    }
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::ParseResponseLine()
{
    char* p = Win() + m_parsePos;
    char* end = static_cast<char*>(memchr(p, '\r', m_bufLen - m_parsePos));
    // Only HTTP/1.0 and HTTP/1.1 have the framing implemented by this connection.
    // Keep the reason phrase borrowed, including an empty phrase.
    //
    if (!end || end - p < 13 || memcmp(p, "HTTP/1.", 7) ||
        (p[7] != '0' && p[7] != '1') || p[8] != ' ' || p[12] != ' ')
        return false;
    int status = 0;
    for (int i = 9; i < 12; ++i)
    {
        if (p[i] < '0' || p[i] > '9') return false;
        status = status * 10 + p[i] - '0';
    }
    if (status < 100 || status > 599) return false;
    for (char* c = p + 13; c < end; ++c)
        if (!detail::ClientValueChar(static_cast<unsigned char>(*c))) return false;
    m_keepAlive = p[7] == '1';
    m_responseLine.status = status;
    m_responseLine.reason = std::string_view(p + 13, end - (p + 13));
    return true;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::AdvanceToPhase(Phase target)
{
    while (m_phase < target && !m_error)
    {
        switch (m_phase)
        {
            case RESPONSE_LINE: if (!GetResponseLine()) return false; break;
            case HEADERS: SkipHeaders(); break;
            case BODY: if (!SkipBody()) return false; break;
            case DONE: break;
        }
    }
    return !m_error;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::FinishHeaders()
{
    const int status = m_responseLine.status;
    m_upgraded = status == 101 || (m_isConnect && status >= 200 && status < 300);
    if (m_upgraded) m_serverClose = true;

    // Framing-free responses take precedence over representation metadata. In
    // particular a CONNECT tunnel is not a close-delimited HTTP response body.
    //
    if (m_isHead || status < 200 || status == 204 || status == 304 || m_upgraded)
    {
        m_phase = DONE;
        return true;
    }
    // Ambiguous framing must never enter a persistent connection pool. Reject it
    // rather than silently deciding which upstream interpretation to trust.
    //
    if (m_transferEncoding && m_contentLength >= 0) return Fail(-EPROTO);
    m_phase = BODY;
    m_closeDelimited = !m_chunkedBody && (m_transferEncoding || m_contentLength < 0);
    if (m_closeDelimited) m_serverClose = true;
    if (!m_chunkedBody && !m_closeDelimited)
    {
        m_bodyRemaining = static_cast<size_t>(m_contentLength);
        if (!m_bodyRemaining) m_phase = DONE;
    }
    return true;
}

template<typename Derived>
const char* ClientConnectionImpl<Derived>::NextHeaderName()
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
                    if (!detail::ClientToken(static_cast<unsigned char>(start[i])))
                    {
                        Fail(-EPROTO);
                        return nullptr;
                    }
                }
            }
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
            if (!detail::ClientToken(static_cast<unsigned char>(start[i])))
            {
                Fail(-EPROTO);
                return nullptr;
            }
        }
        if (!Receive()) return nullptr;
    }
}

// Framing metadata is recognized incrementally in the very spans exposed to callers.
// Token matching retains only a mask and a saturating length, never an owning copy or
// a header-sized scratch buffer. Arbitrarily long unknown connection/transfer tokens
// therefore cost constant state.
//
template<typename Derived>
bool ClientConnectionImpl<Derived>::FinishHeaderToken()
{
    if (!m_tokenLength) return Fail(-EPROTO);
    bool chunked = (m_tokenMatches & 1) && m_tokenLength == 7;
    if (m_pendingTransferEncoding)
    {
        if (m_chunkedBody) return Fail(-EPROTO); // chunked must occur once, last
        m_chunkedBody = chunked;
        m_transferEncoding = true;
        m_lastTokenChunked = chunked;
    }
    else
    {
        if ((m_tokenMatches & 2) && m_tokenLength == 5) m_serverClose = true;
        if ((m_tokenMatches & 4) && m_tokenLength == 10) m_keepAlive = true;
    }
    m_tokenLength = 0;
    m_tokenMatches = 7;
    return true;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::FeedHeaderValue(const char* data, size_t size, bool final)
{
    // A successful CONNECT ends HTTP framing at the header terminator. RFC 9110
    // requires ignoring CL/TE even if a peer sends malformed framing metadata.
    //
    if (m_isConnect && m_responseLine.status >= 200 && m_responseLine.status < 300 &&
        (m_pendingContentLength || m_pendingTransferEncoding)) return true;
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
                m_serverClose = true;
                return true;
            }
        }
        else if (length == 7 && !strncasecmp(data, "chunked", 7))
        {
            if (m_chunkedBody) return Fail(-EPROTO);
            m_transferEncoding = true;
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
            if (detail::ClientToken(c))
            {
                static constexpr const char* tokens[] = {"chunked", "close", "keep-alive"};
                for (unsigned t = 0; t != 3; ++t)
                {
                    if ((m_tokenMatches & (1u << t)) &&
                        detail::ClientLower(c) != static_cast<unsigned char>(tokens[t][m_tokenLength]))
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
            if (!detail::ClientToken(c)) return Fail(-EPROTO);
            m_fieldState = 4;
        }
        else if (m_fieldState == 4)
        {
            if (detail::ClientToken(c)) continue;
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
            if (!detail::ClientToken(c)) return Fail(-EPROTO);
            m_fieldState = 7;
        }
        else if (m_fieldState == 7)
        {
            if (detail::ClientToken(c)) continue;
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

template<typename Derived>
Chunk* ClientConnectionImpl<Derived>::ReadHeaderValue()
{
    if (m_valueConsumed || m_error) return nullptr;
    while (true)
    {
        if (m_parsePos == m_bufLen && !Receive()) return nullptr;
        if (!m_valueStarted)
        {
            while (m_parsePos < m_bufLen &&
                   (Win()[m_parsePos] == ' ' || Win()[m_parsePos] == '\t')) ++m_parsePos;
            if (m_parsePos == m_bufLen) continue;
            m_valueStarted = true;
        }
        size_t start = m_parsePos;
        size_t end = start;
        while (end < m_bufLen && Win()[end] != '\r')
        {
            if (!detail::ClientValueChar(static_cast<unsigned char>(Win()[end])))
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
void ClientConnectionImpl<Derived>::SkipHeaderValue()
{
    while (auto* chunk = ReadHeaderValue()) if (chunk->complete) break;
}

template<typename Derived>
void ClientConnectionImpl<Derived>::SkipHeaders()
{
    while (NextHeaderName()) SkipHeaderValue();
}

template<typename Derived>
int64_t ClientConnectionImpl<Derived>::ContentLength()
{
    // Always finish metadata before reporting it: later occurrences can contradict an
    // earlier length. -1 means absent (including chunked/close-delimited framing).
    //
    if (m_phase < BODY) AdvanceToPhase(BODY);
    return m_contentLength;
}

template<typename Derived>
Chunk* ClientConnectionImpl<Derived>::ReadBody()
{
    if (m_phase < BODY && !AdvanceToPhase(BODY)) return nullptr;
    if (m_phase != BODY || m_error) return nullptr;
    if (m_chunkedBody) return ReadChunkedBody();
    if (m_parsePos == m_bufLen && !Receive(m_closeDelimited)) return nullptr;
    size_t available = m_bufLen - m_parsePos;
    size_t size = m_closeDelimited ? available : std::min(available, m_bodyRemaining);
    m_chunk = {Win() + m_parsePos, size, false};
    m_parsePos += size;
    if (!m_closeDelimited)
    {
        m_bodyRemaining -= size;
        m_chunk.complete = m_bodyRemaining == 0;
        if (!m_bodyRemaining) m_phase = DONE;
    }
    return &m_chunk;
}

template<typename Derived>
BodyResult ClientConnectionImpl<Derived>::NextBody()
{
    Chunk* chunk = ReadBody();
    return {chunk, m_error};
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::SkipBody()
{
    while (ReadBody()) {}
    return Complete();
}

template<typename Derived>
Chunk* ClientConnectionImpl<Derived>::ReadChunkedBody()
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
                if (!detail::ClientValueChar(c)) { Fail(-EPROTO); return nullptr; }
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
                    if (detail::ClientToken(c)) { extensionState = 2; continue; }
                }
                else if (extensionState == 2)
                {
                    if (detail::ClientToken(c)) continue;
                    if (ows) { extensionState = 3; continue; }
                    if (c == '=') { extensionState = 4; continue; }
                    if (c == ';') { extensionState = 1; continue; }
                }
                else if (extensionState == 4)
                {
                    if (ows) continue;
                    if (c == '"') { extensionState = 7; continue; }
                    if (detail::ClientToken(c)) { extensionState = 5; continue; }
                }
                else if (extensionState == 5)
                {
                    if (detail::ClientToken(c)) continue;
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
                    m_phase = DONE;
                    return nullptr;
                }
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
                    if (name)
                    {
                        if (c == ':' && haveName) name = false;
                        else if (detail::ClientToken(c)) haveName = true;
                        else { Fail(-EPROTO); return nullptr; }
                    }
                    else if (!detail::ClientValueChar(c)) { Fail(-EPROTO); return nullptr; }
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
    // response. NextBody().Complete()/Complete() report message completion after the
    // zero chunk and trailers. The delimiter is consumed only on the next pull.
    //
    m_chunk.complete = m_bodyRemaining == 0;
    m_needChunkCrlf = m_bodyRemaining == 0;
    return &m_chunk;
}

// Disk writes are explicit caller-requested work. Keep their errno instead of masking
// a failed sink as a successful/truncated response.
//
inline int ClientWriteAllToFile(io::Descriptor& file, const void* data, size_t size, off_t offset)
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
int64_t ClientConnectionImpl<Derived>::ReadBodyToFile(int fileFd, off_t offset)
{
    if (m_error) return m_error;
    if (offset < 0) return -EINVAL;
    if (m_phase < BODY && !AdvanceToPhase(BODY)) return m_error;
    if (m_phase != BODY) return 0;
    auto& desc = static_cast<Derived*>(this)->m_transport.Descriptor();
    io::Descriptor file(io::borrowed, fileFd, desc.m_ring);
    int64_t total = 0;

    // Only a fixed-length body permits bounding a kernel splice away from the next
    // response. A recv source already owns the socket stream, so it must be drained
    // through its borrowed windows. TLS and chunked framing use the same pull path.
    //
    if constexpr (Derived::kSpliceable)
    {
        if (!m_chunkedBody && !m_closeDelimited && !this->m_source)
        {
            if (m_bodyRemaining > uint64_t(INT64_MAX - offset)) return -EOVERFLOW;
            size_t buffered = std::min(m_bufLen - m_parsePos, m_bodyRemaining);
            if (buffered)
            {
                int error = ClientWriteAllToFile(file, Win() + m_parsePos, buffered, offset);
                if (error) { Fail(error); return error; }
                m_parsePos += buffered;
                m_bodyRemaining -= buffered;
                total += buffered;
            }
            if (m_bodyRemaining)
            {
                io::PipeLease pipe(desc.m_ring->GetPipePool());
                if (!pipe) { Fail(-EMFILE); return m_error; }
                io::detail::YieldBudget budget(Self());
                while (m_bodyRemaining)
                {
                    int n = io::SpliceToFile(desc, fileFd, offset + total, pipe.Fds(), m_bodyRemaining);
                    if (n <= 0)
                    {
                        pipe.MarkDirty();
                        Fail(n == 0 ? -ECONNRESET : n);
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
            Fail(-EOVERFLOW);
            return m_error;
        }
        int error = ClientWriteAllToFile(file, chunk->data, chunk->size, offset + total);
        if (error) { Fail(error); return error; }
        total += chunk->size;
    }
    return m_error ? m_error : total;
}
// -------------------------------------------------------------------------------------
// Write buffer
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ClientConnectionImpl<Derived>::Append(const void* data, size_t size)
{
    if (m_error) return false;
    if (size == 0) return true;
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
    if (m_error) return false;
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
    if (m_error) return false;
    auto* bytes = static_cast<const char*>(data);
    // Transport counts are int even though the public span is size_t. Keep each
    // operation representable without imposing a maximum on the caller's body.
    //
    while (size)
    {
        size_t count = std::min(size, static_cast<size_t>(INT_MAX));
        int result = TransportSendAll(bytes, count);
        if (result <= 0 || static_cast<size_t>(result) != count)
            return Fail(result < 0 ? result : -EIO);
        bytes += count;
        size -= count;
    }
    return true;
}

// -------------------------------------------------------------------------------------
// Request sending
// -------------------------------------------------------------------------------------

template<typename Derived>
bool ClientConnectionImpl<Derived>::BeginRequest(const char* method, const char* path)
{
    // HEAD responses carry framing headers with no body bytes — remember the method so
    // the response parser knows not to wait for one.
    //
    if (m_error) return false;
    assert(method && path && m_host);
    m_isHead = strcmp(method, "HEAD") == 0;
    m_isConnect = strcmp(method, "CONNECT") == 0;

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
    return AppendLiteral("\r\n");
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::AppendHeader(const char* name, std::string_view value)
{
    if (!Append(name, strlen(name))) return false;
    if (!AppendLiteral(": ")) return false;
    if (!Append(value.data(), value.size())) return false;
    return AppendLiteral("\r\n");
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::AppendHeader(const char* name, size_t value)
{
    if (!Append(name, strlen(name))) return false;
    if (!AppendLiteral(": ")) return false;
    if (!AppendUInt(value)) return false;
    return AppendLiteral("\r\n");
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::EndHeaders()
{
    if (!AppendLiteral("\r\n")) return false;
    return Flush();
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::SendBody(const void* data, size_t size)
{
    if (m_error) return false;
    if (size == 0) return true;
    if (!Append(data, size)) return false;
    return Flush();
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::SendBodyFromFile(int fileFd, off_t offset, size_t count)
{
    if (m_error) return false;
    if (offset < 0) return Fail(-EINVAL);
    if (count > static_cast<uint64_t>(std::numeric_limits<off_t>::max() - offset))
        return Fail(-EOVERFLOW);
    if (!Flush()) return false;
    while (count)
    {
        size_t take = std::min(count, static_cast<size_t>(INT_MAX));
        int result = TransportSendfileAll(fileFd, offset, take);
        if (result <= 0 || static_cast<size_t>(result) != take)
            return Fail(result < 0 ? result : -EIO);
        count -= take;
        offset += take;
    }
    return true;
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::SendRequest(
    const char* method, const char* path,
    const char* contentType,
    const void* body, size_t bodySize)
{
    assert(body || bodySize == 0);
    if (!BeginRequest(method, path)) return false;

    // Media type is optional; payload framing is not. An explicitly supplied empty
    // body/type and the body-oriented convenience methods also emit a zero length.
    //
    if (contentType && !AppendHeader("Content-Type", contentType)) return false;
    if (body || bodySize || contentType || !strcmp(method, "POST") || !strcmp(method, "PUT"))
    {
        if (!AppendHeader("Content-Length", bodySize)) return false;
    }

    // Terminate the header block directly (not EndHeaders — that flushes) so headers
    // and a small body coalesce into one send.
    //
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
bool ClientConnectionImpl<Derived>::Head(const char* path)
{
    return SendRequest("HEAD", path);
}

template<typename Derived>
bool ClientConnectionImpl<Derived>::Post(
    const char* path, const char* contentType,
    const void* body, size_t bodySize)
{
    return SendRequest("POST", path, contentType, body, bodySize);
}

} // namespace http
} // namespace coop
