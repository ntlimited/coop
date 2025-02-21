#include "execution_handle.h"

#include "execution_context.h"

void ExecutionHandle::Kill()
{
    m_executionContext->Kill();
}
