#pragma once

#include "execution_context.h"
#include "scheduler_state.h"
	
template<typename Fn>
bool Manager::Spawn(Fn const& fn, ExecutionHandle* handle /* = nullptr */)
{
    return Spawn(s_defaultConfiguration, fn, handle);
}
	
template<typename Fn>
bool Manager::Spawn(SpawnConfiguration const& config, Fn const& fn, ExecutionHandle* handle /* = nullptr */)
{
    if (m_shutdown || !m_executionContexts.CanPush())
	{
        return false;
	}

	auto* alloc = AllocateExecutionContext(config);
    if (!alloc)
	{
        return false;
	}
	auto* executionContext = new (alloc) ExecutionContext(config, handle, this);

    m_executionContexts.Push(executionContext);

	// Depending on whether we're running this from the manager's own stack or
    // not.
	//
    auto* lastExecuting = m_scheduled;
	auto& buf = (lastExecuting ? lastExecuting->m_jmpBuf : m_jmpBuf);
    bool isSelf = !lastExecuting;

    if (lastExecuting)
    {
        lastExecuting->m_state = SchedulerState::YIELDED;
        m_yielded.Push(lastExecuting);
        m_scheduled = nullptr;
    }

	// Actually start executing.
    //
    auto jmpRet = setjmp(buf);
    if (!jmpRet)
    {
        executionContext->m_state = SchedulerState::RUNNING;
		m_scheduled = executionContext;
        auto* ctx = executionContext;
        auto* entry = &fn;
		void* top = ctx->m_segment.Top();
        asm volatile(
			"mov %[rs], %%rsp \n"
            : [ rs ] "+r" (top) ::
        );
		(*entry)(ctx);

        // When the code in the func finishes, exit the context by resuming into
        // the scheduler's code. Regardless of where it was when it gave up control
        //
        LongJump(Manager::thread_manager->m_jmpBuf, SchedulerJumpResult::EXITED);
        // Unreachable
        //
        assert(false);
    }

    if (isSelf)
    {
        HandleManagerResumption(static_cast<SchedulerJumpResult>(jmpRet));
    }
    else
    {
        assert(lastExecuting == m_scheduled);
    }
    return true;
}
