#include "descriptor.h"

#include "uring.h"

#include "coop/context.h"
#include "coop/cooperator.h"
#include "coop/embedded_list.h"
#include "coop/self.h"

namespace coop
{

namespace io
{

Descriptor::Descriptor(int fd, Uring* ring /* = nullptr */)
: m_ring(ring ? ring : Self()->GetCooperator()->GetUring())
, m_fd(fd)
, m_registered(nullptr)
, m_generation(0)
{
    assert(m_ring);
    m_ring->Register(this);
}

Descriptor::~Descriptor()
{
    m_ring->Unregister(this);
}

int Descriptor::Close()
{
    return close(m_fd);
}

} // end namespace io
} // end namespace coop
