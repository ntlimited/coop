#pragma once

#include "coop/detail/embedded_list.h"
#include "coop/self.h"

namespace coop
{

namespace io
{

struct Handle;
struct Uring;

// Tag type for opting a descriptor into io_uring fd registration
//
struct Registered {};
inline constexpr Registered registered;

// A Descriptor (as in "file descriptor") is our wrapper for file/socket operations. We mirror its
// usage with being a largely passive set of state that is passed into methods which actually do the
// work (see operations.h)
//
struct Descriptor : EmbeddedListHookups<Descriptor>
{
    Descriptor(int fd, Uring* ring = GetUring());
    Descriptor(Registered, int fd, Uring* ring = GetUring());

    ~Descriptor();

    int Close();

    // TODO lock down the guts
    //
    Uring* m_ring;

    int32_t         m_fd;

    // Index into the uring's registered fd table, or -1 if not registered
    //
    friend struct Uring;
    int             m_registeredIndex;

    int             m_result;

    friend struct Handle;
    EmbeddedList<Handle> m_handles;
};

} // end namespace coop::io
} // end namespace coop
