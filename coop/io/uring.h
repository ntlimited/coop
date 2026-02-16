#pragma once

#include <cstring>
#include <liburing.h>
#include <tuple>
#include <utility>
#include <vector>

#include "descriptor.h"
#include "uring_configuration.h"

#include "coop/coordinator.h"
#include "coop/detail/embedded_list.h"

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
    using DescriptorList = EmbeddedList<Descriptor>;

    Uring(UringConfiguration const& config = s_defaultUringConfiguration)
    : m_config(config)
    , m_registered(config.registeredSlots, -1)
    {
        memset(&m_ring, 0, sizeof(m_ring));
    }

    int PendingOps() const { return m_pendingOps; }

    void Init();

    // Process any available CQEs without blocking. Returns the number of CQEs dispatched.
    //
    int Poll();

    void Run(Context* ctx);

    // TODO lock down the guts
    //

    friend struct Descriptor;
    friend struct Handle;

    // Register/Unregister handle kernel fd registration only. The descriptor list is managed
    // directly by Descriptor constructors/destructors.
    //
    void Register(Descriptor*);
    void Unregister(Descriptor*);

    struct  io_uring m_ring;
    DescriptorList m_descriptors;
    int m_pendingOps{0};

    // io_uring fd registration table. Slots contain the real fd or -1 for empty. Registration is
    // opt-in via the Descriptor(Registered, ...) constructor. When a descriptor is registered, its
    // slot index is stored in Descriptor::m_registeredIndex and operations use IOSQE_FIXED_FILE.
    //
    std::vector<int> m_registered;
    UringConfiguration m_config;
};

} // end namespace io

} // end namespace coop
