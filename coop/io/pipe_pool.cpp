#include "pipe_pool.h"

#include <fcntl.h>
#include <unistd.h>

namespace coop
{

namespace io
{

PipePool::~PipePool()
{
    for (int i = 0; i < m_count; i++)
    {
        ::close(m_pipes[i][0]);
        ::close(m_pipes[i][1]);
    }
}

bool PipePool::Acquire(int pipefd[2])
{
    if (m_count > 0)
    {
        m_count--;
        pipefd[0] = m_pipes[m_count][0];
        pipefd[1] = m_pipes[m_count][1];
        return true;
    }
    return ::pipe2(pipefd, O_NONBLOCK) == 0;
}

void PipePool::Release(const int pipefd[2])
{
    if (m_count < kMaxCached)
    {
        m_pipes[m_count][0] = pipefd[0];
        m_pipes[m_count][1] = pipefd[1];
        m_count++;
        return;
    }
    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

void PipePool::Discard(const int pipefd[2])
{
    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

} // end namespace coop::io
} // end namespace coop
