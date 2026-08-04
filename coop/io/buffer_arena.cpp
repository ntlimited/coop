#include "buffer_arena.h"

#include <cassert>
#include <cerrno>
#include <liburing.h>
#include <sys/mman.h>

#include <spdlog/spdlog.h>

#include "uring.h"

namespace coop
{

namespace io
{

BufferArena::BufferArena(size_t bytes)
{
    // Round to page size; ask for transparent huge pages — fewer TLB entries and a
    // shorter bvec for the kernel's registered-buffer bookkeeping. Advisory only.
    //
    size_t page = 4096;
    m_size = (bytes + page - 1) & ~(page - 1);

    void* p = mmap(nullptr, m_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
    {
        spdlog::warn("buffer arena mmap({}) failed: errno={}", m_size, errno);
        m_size = 0;
        return;
    }
    m_base = static_cast<char*>(p);
    (void)madvise(m_base, m_size, MADV_HUGEPAGE);
}

BufferArena::~BufferArena()
{
    // Kernel-side unregistration rides the ring teardown; the mapping just unmaps.
    //
    if (m_base)
    {
        munmap(m_base, m_size);
    }
}

int BufferArena::Register(Uring& uring)
{
    if (!m_base)
    {
        return -ENOMEM;
    }

    iovec iov = { m_base, m_size };
    int ret = io_uring_register_buffers(uring.Ring(), &iov, 1);
    if (ret < 0)
    {
        return ret;
    }
    m_registered = true;
    return 0;
}

char* BufferArena::Acquire(size_t bytes)
{
    if (!m_registered || m_bump + bytes > m_size)
    {
        return nullptr;
    }
    char* p = m_base + m_bump;
    m_bump += bytes;
    return p;
}

void BufferArena::Release(char* p, size_t bytes)
{
    assert(p + bytes == m_base + m_bump && "arena releases must be LIFO");
    m_bump -= bytes;
}

} // end namespace coop::io
} // end namespace coop
