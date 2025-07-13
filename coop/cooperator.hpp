#pragma once

#include <cstring>
#include <type_traits>

#include "context.h"
#include "scheduler_state.h"
#include "self.h"

namespace coop
{

// This is an incredibly cool syntax, but it is incredibly dangerous and misleading too. While it works,
// the nature of lambdas being passed in is that Spawn([&](Context*) { ... }) is stack allocating in a
// way not guaranteed after `Spawn()` returns, e.g. after the control returns to the spawning context
// from the spawned context.
//
// This is not a problem per se
//
template<typename Fn>
bool Cooperator::Spawn(Fn const& fn, Context::Handle* handle /* = nullptr */)
{
    return Spawn(s_defaultConfiguration, fn, handle);
}
    
template<typename Fn>
bool Cooperator::Spawn(SpawnConfiguration const& config, Fn const& fn, Context::Handle* handle /* = nullptr */)
{
    if (m_shutdown || (m_scheduled && m_scheduled->IsKilled()))
    {
        return false;
    }

    auto* alloc = AllocateContext(config);
    if (!alloc)
    {
        return false;
    }

    // After this point we'll count it as time spent in the context
    //
    m_ticks += rdtsc() - m_lastRdtsc;
    auto* spawnCtx = new (alloc) Context(m_scheduled /* parent */, config, handle, this);
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


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
        // TODO: this would need a rework when proper stack handing etc is added, right now it's
        // effective but goofy.
        //
        memcpy(spawnCtx->m_segment.Bottom(), &fn, sizeof(Fn));
#pragma GCC diagnostic pop

        void* top = spawnCtx->m_segment.Top();
        // After this point, we have moved to the stack we allocated
        //
        asm volatile(
            "mov %[rs], %%rsp \n"
            : [ rs ] "+r" (top) ::
        );

        {
            // This is pretty wonderfully obtuse, but it allows us to skip touching any potentially
            // compiled code relying on stack offsets that no longer are valid.
            //
            // Probably there's a nicer way to communicate this fact to the compiler, but another
            // time perhaps?
            //
            (*((Fn*)(Self()->m_segment.Bottom())))(Self());
        }

        // This deletion must be done while the context is still in its own stack, so that it can
        // context switch to anyone waiting on the signal safely. We also can't simply unblock them
        // because then there's an insane contract where you could be signalled for wakeup by a
        // (once you wake up) deallocated Coordinator you're still holding onto.
        //
        // Same rule as above for why we are using Self() here
        //
        Self()->~Context();

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

// TODO dedupe this ridiculous copy/paste from Spawn.
//
template<typename T, typename... Args>
T* Cooperator::Launch(SpawnConfiguration const& config, Context::Handle* handle, Args&&... args)
{
    static_assert(std::is_base_of<Launchable, T>::value);

    if (m_shutdown || (m_scheduled && m_scheduled->IsKilled()))
    {
        return nullptr;
    }

    auto* alloc = AllocateContext(s_defaultConfiguration);
    if (!alloc)
    {
        return nullptr;
    }

    m_ticks += rdtsc() - m_lastRdtsc;
    auto* spawnCtx = new (alloc) Context(m_scheduled /* parent */, config, handle, this);
    m_contexts.Push(spawnCtx);

    // Currently this has no reason to ever get invoked from the cooperator itself.
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
    spawnCtx->m_state = SchedulerState::RUNNING;
    m_scheduled = spawnCtx;

    SanityCheck();

    auto* addr = spawnCtx->m_segment.Bottom();
    auto* launchable = new (spawnCtx->m_segment.Bottom()) T(
        spawnCtx,
        std::forward<Args&&>(args)...
    );

    void* top = spawnCtx->m_segment.Top();

    // Actually start executing.
    //
    auto jmpRet = setjmp(buf);
    if (!jmpRet)
    {
        asm volatile(
            "mov %[rs], %%rsp \n"
            : [ rs ] "+r" (top) ::
        );

        {
            reinterpret_cast<T*>(Self()->m_segment.Bottom())->Launch();
        }

        // This deletion must be done while the context is still in its own stack, so that it can
        // context switch to anyone waiting on the signal safely. We also can't simply unblock them
        // because then there's an insane contract where you could be signalled for wakeup by a
        // (once you wake up) deallocated Coordinator you're still holding onto.
        //
        Self()->~Context();

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

    return launchable;;
}

} // end namespace coop
