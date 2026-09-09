#include "connection.h"
#include "transport.h"
#include "tls_transport.h"
#include "detail/server_impl.hpp"

namespace coop::http
{
template struct ConnectionImpl<Connection<PlaintextTransport>>;
template struct ConnectionImpl<Connection<TlsTransport>>;
}
