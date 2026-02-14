#pragma once

#include <cstring>
#include <liburing.h>
#include <tuple>
#include <utility>

#include "descriptor.h"

#include "coop/coordinator.h"
#include "coop/embedded_list.h"

namespace coop
{

struct Context;

namespace io
{

// the coop::io::Uring serves as a wrapper around io_uring, as wrapped by liburing. It is not
// expected that most developers will need to interact with the uring in any direct fashion:
// instead, the
//
struct Uring
{
    static constexpr size_t REGISTERED_SIZE = 64;

    using DescriptorList = EmbeddedList<Descriptor>;

    Uring(int entries)
    : m_entries(entries)
    , m_registeredCount(0)
    , m_cleanGeneration(0)
    , m_dirtyGeneration(1)
    {
        memset(&m_ring, 0, sizeof(m_ring));

        for (int i = 0 ; i < REGISTERED_SIZE ; i++)
        {
            m_registered[i] = -1;
        }
    }

    void Init()
    {
        int ret = io_uring_queue_init(m_entries, &m_ring, 0);
        assert(ret == 0);
        std::ignore = ret;
    }

    void Run(Context* ctx);

    // TODO lock down the guts
    //

    friend struct Descriptor;
    void Register(Descriptor*);
    void Unregister(Descriptor*);
    int RegisteredIndex(int* at);

    struct  io_uring m_ring;
    DescriptorList m_descriptors;

    // TODO: io_uring has (optional) epoll-esque registration semantics which allows for less
    // overhead over time as kernel structures can be maintained. This is something I'm still
    // feeling out as to how it works; in short, we have a "best effort" system where if we have
    // space in our (statically allocated) array of registered fds, we just use it. Descriptors
    // get added automatically to the registered set if there is space, but because this is
    // (1) a syscall and (2) batchable, we defer it. This is done by tracking "dirty" and "clean"
    // generation markers: when a new descriptor is 'registered', we set `m_generation` to
    // the dirty generation on the uring. If we attempt to operate on a descriptor whose
    // generation is greater than the "clean" generation, it isn't truly registered and we use
    // the FD as-is.
    //
    // When the uring wakes, we check if we are dirty and then perform the registration syscall
    // and then move the clean generation marker up.
    //
    // For unregistering, we simply increment the dirty generation marker so that we will pick it
    // up later.
    //
    // The "TODO" part is that this is almost certainly unoptimal and could be heavily optimized.
    // Would it have been better not to bother? Who can say.
    //
    // In terms of making this work, we need to start by making it easier to replace elements:
    // in our toolkit, this would take place as a pair of embedded lists tracking (1) available
    // slots and (2) dirty changes that need to be flushed. Harder is optimizing how the refresh
    // works; i'm not entirely sure how expensive various optimizable portions of the refresh
    // syscall are.
    //
    int32_t m_registered[REGISTERED_SIZE];
    int     m_entries;
    int     m_registeredCount;
    int     m_cleanGeneration;
    int     m_dirtyGeneration;
};

} // end namespace io

} // end namespace coop
