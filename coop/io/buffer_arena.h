#pragma once

#include <cstddef>
#include <cstdint>

namespace coop
{

namespace io
{

struct Uring;

// BufferArena: one anonymous slab registered with the ring as a single fixed buffer
// (buf_index 0). Registered buffers remove the per-IO get_user_pages/pin work from the
// disk legs (READ_FIXED/WRITE_FIXED) and the zero-copy send leg (SEND_ZC with a fixed
// buffer); there is no registered-buffer recv in any released kernel, so the socket
// read path is unaffected by design.
//
// The arena is deliberately minimal: Base/Size/Index plus a LIFO bump lease, enough for
// bounce buffers and staged bodies. Sub-allocation policy richer than LIFO (per-size
// pools, cross-context ownership) belongs to the consumer or a later layer. Memory is
// charged against RLIMIT_MEMLOCK at registration — if registration fails, the arena
// reports Unavailable and callers fall back to unregistered buffers (the probe idiom).
//
struct BufferArena
{
    BufferArena(BufferArena const&) = delete;
    BufferArena& operator=(BufferArena const&) = delete;

    // Maps (but does not register) the slab. Register() completes setup against a ring.
    //
    explicit BufferArena(size_t bytes);
    ~BufferArena();

    // Register the slab with the ring as fixed buffer 0. Returns negative errno on
    // failure (kernel support, RLIMIT_MEMLOCK); the arena then stays Unavailable.
    //
    int Register(Uring& uring);

    bool Available() const { return m_registered; }
    char* Base() const { return m_base; }
    size_t Size() const { return m_size; }
    uint16_t Index() const { return 0; }

    // LIFO bump lease. Returns nullptr when exhausted or unavailable. Release must be
    // called in reverse acquisition order (asserted), mirroring the context bump heap.
    //
    char* Acquire(size_t bytes);
    void Release(char* p, size_t bytes);

private:
    char*   m_base = nullptr;
    size_t  m_size = 0;
    size_t  m_bump = 0;
    bool    m_registered = false;
};

} // end namespace coop::io
} // end namespace coop
