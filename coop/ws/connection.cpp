#include "connection.h"
#include "coop/http/transport.h"
#include "coop/http/tls_transport.h"
#include "detail/connection_impl.hpp"

namespace coop::ws
{
template struct ConnectionImpl<Connection<http::PlaintextTransport>>;
template struct ConnectionImpl<Connection<http::TlsTransport>>;
}
