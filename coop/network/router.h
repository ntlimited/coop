#pragma once

#include "event_mask.h"
#include "handle.h"

#include "coop/embedded_list.h"
#include "coop/launchable.h"

namespace coop
{

struct Context;
struct Coordinator;

namespace network
{

struct Handle;

// A Router uses an underlying async mechanism to dispatch events, such as epoll, select, etc.
//
// Routers work on Handle instances, and implementations must define three methods describing
// the lifecycle of Handle instances:
//
// Register when the Handle is added to the router
//
// Update when the Handle is modified (switching to caring about write events, presumably)
//
// Unregister when the Handle is removed from the router
//
// A Router must outlive all of its Handles, and all Handles must outlive the router until they are
// unregistered.
//
struct Router : Launchable
{
    Router(Context* ctx)
    : m_context(ctx)
    {
    }

    virtual ~Router()
    {
    }

    virtual void Launch() = 0;

    // Users register handles against a router and then interact with the handle from then on.
    //
    virtual bool Register(Handle*) = 0;

  protected:
    Coordinator* GetCoordinator(Handle* h) const;
    Handle::Data& GetData(Handle* h) const;
    
    void SetRouter(Handle* h);

    friend Handle;
    virtual bool Unregister(Handle*) = 0;
    virtual bool Update(Handle*, const EventMask) = 0;

    Context* m_context;

    using HandleList = EmbeddedList<Handle>;
    HandleList m_list;
};

} // end namespace coop::network

} // end namespace coop
