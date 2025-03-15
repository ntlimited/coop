#include <fcntl.h>

#include "event_mask.h"
#include "handle.h"
#include "router.h"

#include "coop/embedded_list.h"
#include "coop/coordinator.h"
#include "coop/launchable.h"

namespace coop
{

struct Context;

namespace network
{

struct Router;

struct TCPHandler : Launchable
{
    TCPHandler(Context* ctx, int fd);

    virtual ~TCPHandler();

    bool Register(Router* r);

    // TODO another way of doing this would be to just let the TCPServer template on the handler
    // and then stack-instantiate it on the context...
    //
    virtual void Launch() final;

    virtual bool Recv(Context* ctx, void* buffer, const size_t bytes) = 0;

    bool Send(Context* ctx, void* buffer, const size_t bytes);

  protected:
    Handle& GetHandle()
    {
        return m_handle;
    }

  private:
    Coordinator m_coordinator;
    Handle      m_handle;
};

} // end namespace coop::network
} // end namespace coop
