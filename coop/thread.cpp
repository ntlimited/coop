#include <pthread.h>
#include <sched.h>

#include "cooperator.h"
#include "thread.h"

namespace coop
{

namespace
{

void ThreadTarget(Cooperator* mgr)
{
    mgr->Launch();
}

} // end anonymous namespace

Thread::Thread(Cooperator* c)
: m_cooperator(c)
, m_thread(&ThreadTarget, c)
{
}

Thread::~Thread()
{
    m_thread.join();
}

bool Thread::PinToCore(int core)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(m_thread.native_handle(), sizeof(cpuset), &cpuset) == 0;
}

} // end namespace coop
