#include "coop/chan/channel.h"
#include "coop/self.h"

namespace coop
{
namespace chan
{

bool BaseChannel::Shutdown()
{
    return ShutdownEnds(m_shutdown, m_recv, m_send);
}

} // namespace chan
} // namespace coop
