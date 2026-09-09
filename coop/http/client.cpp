#include "client.h"
#include "transport.h"
#include "tls_transport.h"
#include "detail/client_impl.hpp"

namespace coop
{
namespace http
{
template struct ClientConnectionImpl<ClientConnection<PlaintextTransport>>;
template struct ClientConnectionImpl<ClientConnection<TlsTransport>>;
} // namespace http
} // namespace coop
