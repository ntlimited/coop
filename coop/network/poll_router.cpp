#include <cerrno>
#include <string.h>
#include <poll.h>

#include "poll_router.h"

#include "handle.h"

#include "coop/context.h"

namespace coop
{

namespace network
{

PollRouter::PollRouter(Context* ctx)
: Router(ctx)
{
    ctx->SetName("PollRouter");
}

void PollRouter::Iterate(EmbeddedList<Handle>::Iterator& iter)
{
    constexpr size_t lim = 128;
    pollfd fds[lim];
    int i = 0;
    
    auto begin = iter;
    while (i < lim && iter != m_list.end())
    {
        fds[i++] = pollfd{
            iter->GetFD(),
            MaskConverter()(iter->GetMask()),
            0
        };
        ++iter;
    }

    int ret = poll(&fds[0], i, 0 /* return immediately */);
    if (ret < 0)
    {
        printf("Error with poll: %d (%s)\n", errno, strerror(errno));
    }

    int checking = 0;
    while (checking < i && ret)
    {
        if (fds[checking].revents)
        {
            if (GetCoordinator(*begin)->IsHeld())
            {
                printf("Signaling fd %d\n", fds[checking].fd);
                GetCoordinator(*begin)->Release(m_context);
            }
            else
            {
                printf("%d missed wakeup\n", fds[checking].fd);
            }
            ret--;
        }
        ++checking;
        ++begin;
    }
}

void PollRouter::Launch()
{
    while (!m_context->IsKilled())
    {
        auto it = m_list.begin();
        while (it != m_list.end())
        {
            Iterate(it);
        }

        m_context->Yield();
    }
}

bool PollRouter::Register(Handle* handle)
{
    SetRouter(handle);
    return true;
}

bool PollRouter::Unregister(Handle* handle)
{
    return true;
}

bool PollRouter::Update(Handle* handle, const EventMask mask)
{
    return true;
}

} // end namespace coop::network
} // end namespace coop
