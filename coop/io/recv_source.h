#pragma once

#include <cstddef>
#include <cstdint>

#include "armed_handle.h"

#include "coop/coordinator.h"

namespace coop
{

struct Context;

namespace io
{

struct BufferRing;
struct Descriptor;

// RecvSource: a span-oriented consumer facade over multishot recv (ArmedHandle + a
// provided-buffer ring). One armed SQE serves the connection's lifetime; bytes surface
// as kernel-selected chunk spans that callers Peek and Consume — partial consumption is
// retained across Peeks, so a parser can take exactly what a token needs. The span from
// Peek stays valid until the call after it is fully consumed (the underlying chunk is
// recycled when the NEXT chunk is fetched, mirroring ArmedHandle::Next).
//
// -ENOBUFS from a drained pool re-arms transparently (bounded retries) and is counted;
// a pool smaller than the in-flight demand surfaces as -ENOBUFS after the retries.
//
struct RecvSource
{
    RecvSource(Context* ctx, Descriptor& desc, BufferRing* ring)
    : m_armed(ctx, desc, ring, &m_coord)
    {
        m_armed.Arm();
    }

    // Current unconsumed span, blocking for bytes when none: > 0 span length (*data
    // set), 0 EOF, < 0 negative errno.
    //
    int Peek(char** data);

    // Mark n bytes of the current span consumed (n bounded by the last Peek).
    //
    void Consume(size_t n);

    uint64_t Enobufs() const { return m_enobufs; }

private:
    Coordinator m_coord;
    ArmedHandle m_armed;

    char*    m_data = nullptr;
    size_t   m_len = 0;
    size_t   m_off = 0;
    bool     m_eof = false;
    int      m_err = 0;
    uint64_t m_enobufs = 0;
};

} // end namespace coop::io
} // end namespace coop
