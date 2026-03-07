#include "coop/chan/channel.h"
#include "coop/self.h"

namespace coop
{
namespace chan
{

bool BaseChannel::Shutdown()
{
    if (m_shutdown)
    {
        return false;
    }

    m_shutdown = true;
    Context* ctx = Self();

    if (m_recv.IsHeld())
    {
        m_recv.Release(ctx);
    }
    if (m_send.IsHeld())
    {
        m_send.Release(ctx);
    }

    return true;
}

} // namespace chan
} // namespace coop
