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

} // end namespace coop
