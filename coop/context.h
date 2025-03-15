#pragma once

#include <csetjmp>
#include <cstdint>

#include "coordinator.h"
#include "channel.h"
#include "embedded_list.h"
#include "scheduler_state.h"
#include "spawn_configuration.h"

namespace coop
{

struct Segment
{
	size_t m_size;
	uint8_t	m_bottom[0] __attribute__((aligned(128)));
	
	size_t Size() const
	{
		return m_size;
	}

	void* Bottom()
	{
		return reinterpret_cast<void*>(&m_bottom[0]);
	}

	void* Top()
	{
		return reinterpret_cast<void*>(&m_bottom[m_size]);
	}
};

struct Coordinator;
struct CoordinatorExtension;
struct Cooperator;

// Three different groups of mutually exclusive lists are kept for contexts:
// - the list of all contexts for a given cooperator
// - the list of all contexts in a given state for a given cooperator
// - the list of all child contexts for a given cooperator
//
static constexpr int CONTEXT_LIST_ALL = 0;
static constexpr int CONTEXT_LIST_STATE = 1;
static constexpr int CONTEXT_LIST_CHILDREN = 2;

// An Context is what code runs "in," in cooperation with a Cooperator. Each
//
struct Context : EmbeddedListHookups<Context, int, CONTEXT_LIST_ALL>
               , EmbeddedListHookups<Context, int, CONTEXT_LIST_STATE>
               , EmbeddedListHookups<Context, int, CONTEXT_LIST_CHILDREN>
{
    // Embedded lists for tracking the set of lists that contexts can never be in more
    // than one of, e.g. there are multiple `ContextStateList`s in the cooperator, but contexts are
    // never in both active and yielded, etc. Or in two different cooperator's active list...
    //
    using AllContextsList = EmbeddedList<Context, int, CONTEXT_LIST_ALL>;
    using ContextStateList = EmbeddedList<Context, int, CONTEXT_LIST_STATE>;
    using ContextChildrenList = EmbeddedList<Context, int, CONTEXT_LIST_CHILDREN>;

    struct Handle;

  protected:
    friend struct Cooperator;

	Context(
        Context* parent,
        SpawnConfiguration const& config,
        Handle* handle,
        Cooperator* cooperator);
  public:

    ~Context();

    // Return control to the cooperator so that it can schedule other contexts
    //
    bool Yield(bool force = false);

    Cooperator* GetCooperator()
    {
        return m_cooperator;
    }

    // The Killed system for contexts (and presumably most things) functions using a coordinator
    // that initially is held by the context itself, and then shutdown as a kill signal.
    //
    bool IsKilled() const
    {
        return !m_killedCoordinator.IsHeld();
    }

    // The KilledSignal 
    //
    Coordinator* GetKilledSignal()
    {
        return &m_killedCoordinator;
    }

    Context* Parent()
    {
        return m_parent;
    }

    const char* GetName() const
    {
        return m_name ? m_name : "[anonymous]";
    }

    void SetName(const char* name)
    {
        m_name = name;
    }

    // Detach disassociates the context from its parent so that it will not be killed when the
    // parent is. If you have a reason to use this, it is assumed you're able to make sure no
    // there's a coherent contract where no one else is going to or already detached it.
    //
    void Detach();

  private:
    friend struct Cooperator;
    friend struct Coordinator;
    friend struct CoordinatorExtension;

    // Enter the block caused by the given coordinator
    //
    void Block();

    // Unblock the given context, switching to it if requested.
    //
    void Unblock(Context* c, const bool schedule = true);

  private:
    friend struct Handle;
    
    // Keeping with the concept that all Context APIs should only be called when it is actively
    // scheduled in the cooperator, this kills the other given context. In practice, it's also
    // simply necessary that we leverage the current context to coordinate downstream events.
    //
    void Kill(Context* other, const bool schedule = true);

  public:
    Context* m_parent;
    Handle* m_handle;
	SchedulerState m_state;
	int m_priority;
	int m_currentPriority;
    Cooperator* m_cooperator;
    Coordinator m_killedCoordinator;
    ContextChildrenList m_children;
    Coordinator m_lastChild;
    const char* m_name;

    struct
    {
        size_t ticks;
        size_t yields;
        size_t blocks;
    } m_statistics;
    int64_t m_lastRdtsc;

    // The jmp_buf operates as the 'bookmark' to jump back into when the context is active.
    //
	std::jmp_buf m_jmpBuf;

	// Must be last member
	//
	Segment m_segment;
};

// Handle is the mechanism for working with contexts executing in a cooperator, both in and outsidee
// of cooperating contexts. Handle lifetimes must be guaranteed for as long as the execution of the
// spawned context.
//
// In theory, this is the obvious mechanism to add "return things" to the spawn concept. However,
// that's only really needed for outside-of-cooperator work that will probably want fancier ways
// to do things in general than what's easy to do right now, and the intra-context case, there
// isn't really any sufficiently magic syntax beyond union hijinks.
//
struct Context::Handle
{
    Handle(Handle const&) = delete;
    Handle(Handle&&) = delete;
    Handle()
    {
    }

    void Kill();

    Coordinator* GetKilledSignal();

    operator bool() const
    {
        return !!m_context;
    }

  private:
    friend Context;
    friend Cooperator;
	Context* m_context;
};

} // end namespace coop
