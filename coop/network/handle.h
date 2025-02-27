#pragma once

#include <cstdint>

#include "event_mask.h"

#include "coop/embedded_list.h"

namespace coop
{

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
// Note that handles are completely pure eventing constructs.
//
struct Handle : EmbeddedListHookups<Handle>
{
    // The embedded list for handles is used for tracking which handles are registered to which
    // routers
    //
    using List = EmbeddedList<Handle>;

    Handle(int fd, EventMask mask, Coordinator* coordinator);
    Handle(Handle const&) = delete;
    Handle(Handle&&) = delete;

    ~Handle();

    bool SetNonBlocking();

    void Unregister();

    bool Registered() const;

    EventMask GetMask() const;

    int GetFD() const;

    bool SetMask(EventMask mask);

    bool operator+=(EventMask mask);

    bool operator-=(EventMask mask);

  private:
    friend Router;
    void SetRouter(Router* router);
    Coordinator* GetCoordinator();

    int             m_fd;
    Coordinator*    m_coordinator;
    EventMask       m_mask;
    Router*         m_router;

    union {
        float       f32;
        double      f64;
        uint64_t    u64;
        int64_t     i64;
        void*       ptr;
    } m_data;
};

} // end namespace coop::network

} // end namespace coop
