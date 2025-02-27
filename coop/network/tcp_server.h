#include <utility>
#include <fcntl.h>
#include <type_traits>

#include "event_mask.h"
#include "handle.h"
#include "router.h"

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/coordinator.h"
#include "coop/launchable.h"

namespace coop
{

struct Context;

namespace network
{

struct Router;
struct TCPHandler;

// TCPServer is templated on a handler type which does the work of spawning new contexts for each
// connection accepted.
//
template<typename Handler, typename Arg>
struct TCPServer : Launchable
{
    // Handler must be an implementation of TCPHandler which among other things means it can be10
    //
    static_assert(std::is_base_of<TCPHandler, Handler>::value);
    TCPServer(int fd, Arg arg)
    : m_router(nullptr)
    , m_arg(std::move(arg))
    , m_handle(fd, IN | HUP | ERR, &m_coordinator)
    {
    }

    ~TCPServer()
    {
        m_handle.Unregister();
        close(m_handle.GetFD());
    }

    bool Register(Router* r)
    {
        if (!m_handle.SetNonBlocking())
        {
            return false;
        }

        if (!r->Register(&m_handle))
        {
            return false;
        }
    
        m_router = r;
        return true;
    }

    void Launch(Context* ctx) final
    {
        ctx->SetName("TCPServer");
        while (!ctx->IsKilled())
        {
            int fd = accept(m_handle.GetFD(), nullptr, nullptr);
            if (fd >= 0)
            {
                if (!ctx->GetCooperator()->Spawn([&](Context* handlerContext)
                {
                    // Instantiate the handler on itself and then launch it per the usual interface
                    //
                    Handler h(fd, m_arg);
                    if (!h.Register(m_router))
                    {
                        close(fd);
                        return;
                    }
                    h.Launch(handlerContext);
                    close(fd);
                }))
                {
                    close(fd);
                }
                continue;
            }

            if (errno & (EAGAIN | EWOULDBLOCK))
            {
                m_coordinator.Acquire(ctx);
                continue;
            }

            break;
        }
    }

  private:
    Router*             m_router;
    Arg                 m_arg;
    Coordinator         m_coordinator;
    Handle              m_handle;


};

} // end namespace coop::network

} // end namespace coop
