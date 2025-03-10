#pragma once

#include <vector>

#include <sys/poll.h>

#include "router.h"

namespace coop
{

struct Context;
struct Coordinator;

namespace network
{

struct PollRouter : Router
{
    using MaskConverter = InOutHupErrPri<short,POLLIN,POLLOUT,POLLHUP,POLLERR,POLLPRI>;

    PollRouter(Context* ctx);

    void Launch() final;

    bool Register(Handle*) final;

    bool Unregister(Handle*) final;

    bool Update(Handle*, const EventMask) final;

  private:
    void Iterate(EmbeddedList<Handle>::Iterator&);
};

} // end namespace network

} // end namespace coop
