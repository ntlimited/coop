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

#include "coop/io/recv_source.h"

namespace coop
{

namespace http
{
namespace detail
{

template<typename Derived>
struct ParserBuffer
{
    // Attach a pbuf-chunk recv source BEFORE the first parse. With a source attached the
    // window may point directly into a kernel-selected buffer (zero-copy for anything
    // that fits one chunk); the connection's own buffer becomes reassembly staging for
    // tokens that span chunks. Without one, behavior is the classic contiguous buffer.
    //
    void AttachRecvSource(io::RecvSource* source) { m_source = source; }

  protected:
    // Buffer fill state and the epoch bumped whenever buffered bytes move (Compact) —
    // memoized string_views into the buffer compare epochs to detect staleness.
    //
    size_t   m_bufLen{0};
    size_t   m_parsePos{0};
    uint32_t m_bufEpoch{0};

    // The parse window base: null = the Derived's own buffer (classic mode and staging);
    // non-null = a borrowed pbuf chunk span.
    //
    char*           m_winBase{nullptr};
    io::RecvSource* m_source{nullptr};

    // All parsing reads and in-place tokenization go through Win() — the seam that lets
    // the same parser run over its own buffer or a borrowed chunk.
    //
    char* Win()
    {
        return m_winBase ? m_winBase : static_cast<Derived*>(this)->RecvBuf();
    }

    int RecvMore()
    {
        auto* self = static_cast<Derived*>(this);

        if (self->RecvAborted()) return -1;

        if (!m_source)
        {
            // Classic: one-shot transport recv appending into the own buffer.
            //
            if (m_bufLen >= self->RecvBufSize())
            {
                Compact();
                if (m_bufLen >= self->RecvBufSize()) return 0;
            }

            int n = self->TransportRecv(self->RecvBuf() + m_bufLen,
                                        self->RecvBufSize() - m_bufLen, 0,
                                        self->m_timeout);

            if (self->RecvAborted()) return -1;
            if (n <= 0) return -1;

            m_bufLen += n;
            return n;
        }

        // Pbuf mode. If parsing directly in a chunk, retire it first (remainder moves to
        // staging); then either parse the next chunk in place (staging empty — the
        // zero-copy fast path) or append into staging for cross-chunk reassembly.
        //
        if (m_winBase)
        {
            Compact();
        }
        if (m_bufLen >= self->RecvBufSize())
        {
            return 0;   // staging full: token larger than the reassembly buffer
        }

        char* data = nullptr;
        int n = m_source->Peek(&data);
        if (self->RecvAborted()) return -1;
        if (n <= 0) return -1;

        if (m_bufLen == 0)
        {
            ++m_bufEpoch;
            m_winBase = data;
            m_parsePos = 0;
            m_bufLen = static_cast<size_t>(n);
            return n;
        }

        size_t space = self->RecvBufSize() - m_bufLen;
        size_t take = static_cast<size_t>(n) < space ? static_cast<size_t>(n) : space;
        memcpy(self->RecvBuf() + m_bufLen, data, take);
        m_source->Consume(take);
        m_bufLen += take;
        return static_cast<int>(take);
    }

    void Compact()
    {
        auto* self = static_cast<Derived*>(this);

        if (m_winBase)
        {
            // Retire the borrowed chunk: unconsumed remainder (bounded by staging — an
            // overflow surfaces as RecvMore()'s buffer-full result, matching the classic
            // too-long-token path) is copied into the own buffer, the whole span is
            // consumed, and the window returns to staging.
            //
            ++m_bufEpoch;

            size_t remaining = m_bufLen - m_parsePos;
            size_t copy = remaining < self->RecvBufSize() ? remaining
                                                          : self->RecvBufSize();
            if (copy > 0)
            {
                memmove(self->RecvBuf(), m_winBase + m_parsePos, copy);
            }
            m_source->Consume(m_parsePos + copy);
            m_winBase = nullptr;
            m_bufLen = copy;
            m_parsePos = 0;
            return;
        }

        if (m_parsePos == 0) return;

        // Bytes before m_parsePos — including any request/response line the memoized
        // views point into — are discarded here. The epoch bump lets the memoizing
        // getters detect that their views no longer reference live bytes.
        //
        ++m_bufEpoch;

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
