#pragma once

#include "coop/cooperator.h"
#include "coop/network/handle.h"

namespace coop
{

struct Context;

namespace network
{

struct Router;

// Socket is a thin wrapper over "blocking" send/receive.
//
struct Socket
{
    Socket(int fd);
    ~Socket();

    bool Register(Router* router);

    int Recv(Context* context, void* buffer, size_t len, int flags = MSG_DONTWAIT);;

    int Send(Context* context, const void* buffer, size_t len, int flags = 0);

    // SendAll is a flavor of Send that loops until the entire buffer is flushed.
    //
    int SendAll(Context* context, const void* buffer, size_t len, int flags = 0);

    // Allows eventing in conjunction with other sockets or coordination systems
    //
    Coordinator* GetCoordinator();

  private:
    Coordinator     m_coord;
    Handle          m_handle;
};

} // end namespace coop::network
} // end namespace coop
