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

bool ::coop::IsShuttingDown()
{
    return Cooperator::thread_cooperator->IsShuttingDown();
}

::coop::Cooperator* ::coop::GetCooperator()
{
    return Cooperator::thread_cooperator;
}

::coop::io::Uring* ::coop::GetUring()
{
    return Cooperator::thread_cooperator->GetUring();
}
