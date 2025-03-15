#include "uring.h"

#include "handle.h"

#include "coop/context.h"

namespace coop
{

namespace io
{

// Work loop
//
void Uring::Launch()
{
    while (!GetContext()->IsKilled())
    { 
        GetContext()->Yield();
        
        struct io_uring_cqe* cqe;
        int ret = io_uring_peek_cqe(&m_ring, &cqe);
        if (ret == -EAGAIN || ret == -EINTR)
        {
            continue;
        }
        if (ret < 0)
        {
            assert(false);
        }
        assert(ret == 0);
        Handle::Callback(GetContext(), cqe);
    }
}

// TODO migrate this to an optional system and sidestep a lot of crap...
//
void Uring::Register(Descriptor* descriptor)
{
    m_descriptors.Push(descriptor);
    if (m_registeredCount == REGISTERED_SIZE)
    {
        // Nothing else to do, we're just gonna be on the slowpath
        //
        return;
    }

    int i = 0;
    while (true)
    {
        if (m_registered[i] == -1)
        {
            m_registered[i] = descriptor->m_fd;
            descriptor->m_registered = &m_registered[i];
            break;
        }
        ++i;
    }
    descriptor->m_generation = ++m_dirtyGeneration;
    ++m_registeredCount;
}

int Uring::RegisteredIndex(int* reg)
{
    return reg - &m_registered[0];
}

void Uring::Unregister(Descriptor* descriptor)
{
    m_descriptors.Remove(descriptor);
    if (descriptor->m_registered)
    {
        *descriptor->m_registered = -1;
        ++m_dirtyGeneration;
        --m_registeredCount;
    }
}

} // end namespace io
} // end namespace coop
