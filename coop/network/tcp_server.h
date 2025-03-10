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
    TCPServer(Context* ctx, int fd, Arg arg)
    : m_context(ctx)
    , m_router(nullptr)
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

    void Launch() final
    {
        while (!m_context->IsKilled())
        {
            int fd = accept(m_handle.GetFD(), nullptr, nullptr);
            if (fd >= 0)
            {
                bool spawned = m_context->GetCooperator()->Spawn([&](Context* handlerContext)
                {
                    // Instantiate the handler on itself and then launch it per the usual interface
                    //
                    Handler h(handlerContext, fd, m_arg);
                    if (!h.Register(m_router))
                    {
                        close(fd);
                        return;
                    }
                    h.Launch();
                    close(fd);
                });

                if (!spawned)
                {
                    close(fd);
                }
                continue;
            }

            if (errno & (EAGAIN | EWOULDBLOCK))
            {
                printf("Waiting on signal for new connection\n");
                m_coordinator.Acquire(m_context);
                printf("Signal acquired\n");
                continue;
            }

            break;
        }
    }

  private:
    Context*            m_context;
    Router*             m_router;
    Arg                 m_arg;
    Coordinator         m_coordinator;
    Handle              m_handle;


};

} // end namespace coop::network

} // end namespace coop
