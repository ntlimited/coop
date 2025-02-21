#include <fcntl.h>

#include "event_mask.h"
#include "handle.h"
#include "router.h"

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
    TCPHandler(int fd);

    virtual ~TCPHandler();

    bool Register(Router* r);

    virtual void Launch(Context* ctx) final;
    
    virtual bool Recv(Context* ctx, void* buffer, const size_t bytes) = 0;

    bool Send(Context* ctx, void* buffer, const size_t bytes);

  private:
    int         m_fd;
    Coordinator m_coordinator;
    Handle      m_handle;
};

struct TCPHandlerFactory
{
    virtual ~TCPHandlerFactory() = default;

    virtual TCPHandler* Handler(int fd) = 0;

    virtual void Delete(TCPHandler*) = 0;
};

struct TCPServer : Launchable
{
    TCPServer(int fd, TCPHandlerFactory* factory);

    ~TCPServer();

    bool Register(Router* r);

    void Launch(Context* ctx) final;

  private:
    Coordinator         m_coordinator;
    Handle              m_handle;
    TCPHandlerFactory*  m_factory;
    Router*             m_router;
};

} // end namespace coop::network

} // end namespace coop
