#pragma once

// ParserBuffer<Derived>: the recv-buffer mechanics shared by the server and client HTTP
// parsers — fill (RecvMore), compaction (Compact), and the buffer epoch that detects
// request/response-line view staleness. Both parsers advance a parse position through a
// contiguous buffer refilled in place; this mixin is that shared substrate, and the seam
// where a window abstraction (pbuf-ring chunks) will later slot in beneath both parsers
// at once.
//
// Derived is the FINAL connection type; the parser Impl in between befriends this mixin
// so it can reach the Impl's private buffer/transport helpers (RecvBuf, TransportRecv,
// m_timeout) and its RecvAborted hook — the server checks its context's kill flag there,
// the client has no context and returns false. The Impls pull the members into scope
// with using-declarations (dependent-base name lookup).

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coop
{
namespace http
{
namespace detail
{

template<typename Derived>
struct ParserBuffer
{
  protected:
    // Buffer fill state and the epoch bumped whenever buffered bytes move (Compact) —
    // memoized string_views into the buffer compare epochs to detect staleness.
    //
    size_t   m_bufLen{0};
    size_t   m_parsePos{0};
    uint32_t m_bufEpoch{0};

    int RecvMore()
    {
        auto* self = static_cast<Derived*>(this);

        if (self->RecvAborted()) return -1;

        if (m_bufLen >= self->RecvBufSize())
        {
            Compact();
            if (m_bufLen >= self->RecvBufSize()) return 0;
        }

        int n = self->TransportRecv(self->RecvBuf() + m_bufLen,
                                    self->RecvBufSize() - m_bufLen, 0, self->m_timeout);

        if (self->RecvAborted()) return -1;
        if (n <= 0) return -1;

        m_bufLen += n;
        return n;
    }

    void Compact()
    {
        if (m_parsePos == 0) return;

        // Bytes before m_parsePos — including any request/response line the memoized
        // views point into — are discarded here. The epoch bump lets the memoizing
        // getters detect that their views no longer reference live bytes.
        //
        ++m_bufEpoch;

        auto* self = static_cast<Derived*>(this);
        size_t remaining = m_bufLen - m_parsePos;
        if (remaining > 0)
        {
            memmove(self->RecvBuf(), self->RecvBuf() + m_parsePos, remaining);
        }
        m_bufLen = remaining;
        m_parsePos = 0;
    }
};

} // end namespace coop::http::detail
} // end namespace coop::http
} // end namespace coop
