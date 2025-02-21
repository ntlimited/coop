#pragma once

#include "context.h"
#include "scheduler_state.h"

namespace coop
{

template<typename Fn>
bool Cooperator::Spawn(Fn const& fn, Handle* handle /* = nullptr */)
{
    return Spawn(s_defaultConfiguration, fn, handle);
}
	
template<typename Fn>
bool Cooperator::Spawn(SpawnConfiguration const& config, Fn const& fn, Handle* handle /* = nullptr */)
{
    if (m_shutdown)
	{
        return false;
	}

	auto* alloc = AllocateContext(config);
    if (!alloc)
	{
        return false;
	}
	auto* spawnCtx = new (alloc) Context(config, handle, this);

    m_contexts.Push(spawnCtx);

	// Depending on whether we're running this from the cooperator's own stack or
    // not.
	//
    Context* lastCtx = m_scheduled;
	auto& buf = (lastCtx ? lastCtx->m_jmpBuf : m_jmpBuf);
    bool isSelf = !lastCtx;

    if (lastCtx)
    {
        lastCtx->m_state = SchedulerState::YIELDED;
        m_yielded.Push(lastCtx);
        m_scheduled = nullptr;
    }

	// Actually start executing.
    //
    auto jmpRet = setjmp(buf);
    if (!jmpRet)
    {
        spawnCtx->m_state = SchedulerState::RUNNING;
		m_scheduled = spawnCtx;

        SanityCheck();

        auto* ctx = spawnCtx;
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
        LongJump(Cooperator::thread_cooperator->m_jmpBuf, SchedulerJumpResult::EXITED);
        // Unreachable
        //
        assert(false);
    }

    if (isSelf)
    {
        // If we were in the cooperator itself, same as any other resumption
        //
        HandleCooperatorResumption(static_cast<SchedulerJumpResult>(jmpRet));
    }
    else
    {
        // If we were in a context, we have been scheduled again
        //
        assert(lastCtx == m_scheduled);
    }
    return true;
}

} // end namespace coop
