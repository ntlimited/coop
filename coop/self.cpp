#include "self.h"

#include "context.h"
#include "cooperator.h"

::coop::Context* ::coop::Self()
{
    return Cooperator::thread_cooperator->Scheduled();
}

bool ::coop::Yield()
{
    return ::coop::Self()->Yield();
}

bool ::coop::IsKilled()
{
    return ::coop::Self()->IsKilled();
}

::coop::io::Uring* ::coop::GetUring()
{
    return Cooperator::thread_cooperator->GetUring();
}

::coop::time::Ticker* ::coop::GetTicker()
{
    return Cooperator::thread_cooperator->GetTicker();
}
