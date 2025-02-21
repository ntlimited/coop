#include <fcntl.h>

#include "launchable.h"

struct TCPHandler : Launchable
{
    TCPHandler(int fd)
    : m_fd(fd)
    , m_registered(nullptr)
    {
    }

    virtual ~TCPHandler()
    {
        close(m_fd);
        if (m_registered)
        {
            m_registered->Remove(m_fd);
        }
    }

    bool Register(EpollController* c)
    {
        if (fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL) | O_NONBLOCK) < 0)
        {
            return false;
        }
        if (!c->Add(m_fd, EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR, &m_coordinator))
        {
            return false;
        }

        m_registered = c;
        return true;
    }

    void Launch(ExecutionContext* ctx)
    {
        char buffer[4096];

        while (1)
        {
            int ret = recv(m_fd, &buffer[0], 4096, 0);
            if (ret <= 0)
            {
                if (ret & (EWOULDBLOCK | EAGAIN))
                {
                    // Wait for another signal
                    //
                    m_coordinator.Acquire(ctx);
                    continue;
                }
                break;
            }

            if (!Recv(ctx, &buffer[0], ret))
            {
                break;
            }
        }

      exit:
        delete this;
    }
    
    virtual bool Recv(ExecutionContext* ctx, void* buffer, const size_t bytes) = 0;

    bool Send(ExecutionContext* ctx, void* buffer, const size_t bytes) 
    {
        int remaining = bytes;
        auto* sending = reinterpret_cast<char*>(buffer);

        while (remaining)
        {
            int sent = send(m_fd, sending, remaining, 0);
            if (sent < 0)
            {
                if (sent & (EAGAIN | EWOULDBLOCK))
                {
                    // Wait on the coordinator again
                    //
                    m_coordinator.Acquire(ctx);
                    continue;
                }
                return false;
            }
            remaining -= sent;
            sending += sent;
        }
        return true;
    }

  private:
    int m_fd;
    EpollController* m_registered;
    Coordinator m_coordinator;
};

struct TCPHandlerFactory
{
    virtual ~TCPHandlerFactory() = default;

    virtual TCPHandler* Handler(int fd) = 0;
};

struct TCPServer : Launchable
{
    TCPServer(int fd, TCPHandlerFactory* factory)
    : m_fd(fd)
    , m_factory(factory)
    , m_registered(nullptr)
    {
    }

    ~TCPServer()
    {
        close(m_fd);
        m_registered->Remove(m_fd);
    }

    bool Register(EpollController* c)
    {
        if (fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL) | O_NONBLOCK) < 0)
        {
            return false;
        }

        if (!c->Add(m_fd, EPOLLIN | EPOLLHUP | EPOLLERR, &m_coordinator))
        {
            return false;
        }

        m_registered = c;
        return true;
    }

    void Launch(ExecutionContext* ctx) final
    {
        while (!ctx->IsKilled())
        {
            int fd = accept(m_fd, nullptr, nullptr);
            if (fd >= 0)
            {
                auto* handler = m_factory->Handler(fd);
                if (!handler)
                {
                    close(fd);
                    continue;
                }
                if (!handler->Register(m_registered))
                {
                    printf("Handler failed to register: %d (%s)\n", errno, strerror(errno));
                    delete handler;
                    continue;
                }
                       
                if (!ctx->GetManager()->Launch(*handler))
                {
                    delete handler;
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
    int m_fd;
    TCPHandlerFactory* m_factory;
    EpollController* m_registered;
    Coordinator m_coordinator;
};
