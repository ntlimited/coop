#include "iomgr.h"
#include "manager_thread.h"

namespace
{

void ManagerThreadTarget(Manager* mgr)
{
	mgr->Launch();
}

}

ManagerThread::ManagerThread(Manager* m)
: m_manager(m)
, m_thread(&ManagerThreadTarget, m)
{
}

ManagerThread::~ManagerThread()
{
    m_thread.join();
}
