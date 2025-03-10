#pragma once

#include <cstdint>

#include "event_mask.h"

#include "coop/embedded_list.h"
#include "coop/time/interval.h"

namespace coop
{

struct Context;
struct Coordinator;

namespace network
{

struct Router;

// ::coop::network::Handle allows operating on the router's state for the file descriptor it is a
// handle to. The lifecycle of a handle is:
//
// * The user allocates a handle that pairs a coordinator with a file descriptor, as well as a set
//   of events that the handle should be listening for.
// * The handle is registered against a given router which is scheduled on the active cooperator.
// * The event mask can be modified by various APIs
// * Eventually, the handle is unregistered and it/its associated data can be disposed of safely
//
// Currently, handles carry around the router with them which allows for some neat syntax tricks,
// and also is how the non-abstract parts of the router contract are implemented.
//
// Note that handles are completely pure eventing constructs, it's someone else's job to do the
// actual I/O send/recv operations. This is quite possibly something that will get merged into
// a singular construct with io_uring given the chance but here we are.
//
struct Handle : EmbeddedListHookups<Handle>
{
    // The embedded list for handles is used for tracking which handles are registered to which
    // routers
    //
    using List = EmbeddedList<Handle>;

    Handle(int fd, EventMask mask, Coordinator* coordinator);
    Handle(Coordinator* coordinator)
    : m_fd(-1)
    , m_mask(0)
    , m_coordinator(coordinator)
    {

    }
    Handle(Handle const&) = delete;
    Handle(Handle&&) = delete;

    ~Handle();

    void Configure(int fd, EventMask mask)
    {
        assert(fd >= 0);
        assert(m_fd == -1);
        m_fd = fd;
        m_mask = mask;
    }

    bool SetNonBlocking();

    void Unregister();

    bool Registered() const;

    EventMask GetMask() const;

    int GetFD() const;

    bool SetMask(EventMask mask);

    bool operator+=(EventMask mask);

    bool operator-=(EventMask mask);
    
    bool Wait(Context* ctx);

    bool Wait(Context* ctx, time::Interval i);

    union Data {
        float       f32;
        double      f64;
        uint64_t    u64;
        int64_t     i64;
        void*       ptr;
    };

  private:
    friend Router;
    void SetRouter(Router* router);
    Coordinator* GetCoordinator();
    Data& GetData();

    int             m_fd;
    Coordinator*    m_coordinator;
    EventMask       m_mask;
    Router*         m_router;

    Data m_data;
};

} // end namespace coop::network

} // end namespace coop
