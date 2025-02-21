#pragma once

#include <thread>

namespace coop
{

struct Cooperator;

struct Thread
{
    Thread(Cooperator* m);
    ~Thread();

    Cooperator* m_cooperator;
    std::thread m_thread;
};

}
