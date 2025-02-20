#pragma once

#include <thread>

struct Manager;

struct ManagerThread
{
    ManagerThread(Manager* m);
    ~ManagerThread();

    Manager* m_manager;
    std::thread m_thread;
};
