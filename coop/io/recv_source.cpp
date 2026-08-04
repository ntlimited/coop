#include "recv_source.h"

#include <cassert>
#include <cerrno>

namespace coop
{

namespace io
{

int RecvSource::Peek(char** data)
{
    int enobufsRetries = 0;

    for (;;)
    {
        if (m_off < m_len)
        {
            *data = m_data + m_off;
            return static_cast<int>(m_len - m_off);
        }
        if (m_eof)
        {
            return 0;
        }
        if (m_err != 0)
        {
            return m_err;
        }

        ArmedHandle::Chunk chunk;
        int r = m_armed.Next(&chunk);
        if (r > 0)
        {
            m_data = chunk.data;
            m_len = static_cast<size_t>(r);
            m_off = 0;
            continue;
        }
        if (r == 0)
        {
            m_eof = true;
            return 0;
        }
        if (r == -ENOBUFS && enobufsRetries < 3)
        {
            // The pool drained and the kernel disarmed. Our consumed chunks were
            // recycled by Next(); re-arm and retry — other consumers recycling
            // replenishes the pool as well.
            //
            enobufsRetries++;
            m_enobufs++;
            m_armed.Arm();
            continue;
        }

        m_err = r;
        return r;
    }
}

void RecvSource::Consume(size_t n)
{
    assert(m_off + n <= m_len && "Consume beyond the peeked span");
    m_off += n;
}

} // end namespace coop::io
} // end namespace coop
