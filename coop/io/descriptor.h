#pragma once

#include "coop/detail/embedded_list.h"

namespace coop
{

namespace io
{

struct Handle;
struct Uring;

// A Descriptor (as in "file descriptor") is our wrapper for file/socket operations. We mirror its usage
// with being a largely passive set of state that is passed into methods which actually do the work
// (see operations.h)
//
struct Descriptor : EmbeddedListHookups<Descriptor>
{
    Descriptor(int fd, Uring* ring = nullptr);

    ~Descriptor();

    int Close();

    // TODO lock down the guts
    //
    Uring* m_ring;

    int32_t         m_fd;
    
    // See notes in uring.h
    //
    friend struct Uring;
    int32_t*    m_registered;
    int         m_generation;

    int         m_result;

    friend struct Handle;
    EmbeddedList<Handle> m_handles;
};

} // end namespace coop::io
} // end namespace coop
