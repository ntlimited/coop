#pragma once

#include <type_traits>

#include "context.h"
#include "detail/scheduler_state.h"
#include "self.h"

namespace coop
{

// Typed entry points that know how to invoke the lambda (Spawn) or Launchable (Launch) stored
// at the segment's bottom. Called from CoopContextEntry via ctx->m_entry.
//
template<typename Fn>
void SpawnTrampoline(Context* ctx)
{
    (*reinterpret_cast<Fn*>(ctx->m_segment.Bottom()))(ctx);
}

template<typename T>
void LaunchTrampoline(Context* ctx)
{
    reinterpret_cast<T*>(ctx->m_segment.Bottom())->Launch();
}

template<typename T>
void LaunchCleanup(Context* ctx)
{
    reinterpret_cast<T*>(ctx->m_segment.Bottom())->~T();
}

template<typename Fn>
bool Cooperator::Spawn(Fn const& fn, Context::Handle* handle /* = nullptr */)
{
    return Spawn(s_defaultConfiguration, fn, handle);
}

template<typename Fn>
bool Cooperator::Spawn(SpawnConfiguration const& config, Fn const& fn, Context::Handle* handle /* = nullptr */)
{
    if (m_scheduled && m_scheduled->IsKilled())
    {
        return false;
    }

    SpawnConfiguration actual = config;
    actual.stackSize = StackPool::RoundUpStackSize(config.stackSize);

    auto* alloc = m_stackPool.Allocate(actual.stackSize);
    if (!alloc)
    {
        return false;
    }

    auto* spawnCtx = new (alloc) Context(m_scheduled /* parent */, actual, handle, this);
    m_contexts.Push(spawnCtx);

    assert(sizeof(Fn) <= actual.stackSize);
    new (spawnCtx->m_segment.Bottom()) Fn(fn);

    spawnCtx->m_entry = &SpawnTrampoline<Fn>;
    spawnCtx->m_cleanup = &LaunchCleanup<Fn>;
    EnterContext(spawnCtx);
    return true;
}

template<typename T, typename... Args>
T* Cooperator::Launch(SpawnConfiguration const& config, Context::Handle* handle, Args&&... args)
{
    static_assert(std::is_base_of<Launchable, T>::value);

    if (m_scheduled && m_scheduled->IsKilled())
    {
        return nullptr;
    }

    SpawnConfiguration actual = config;
    actual.stackSize = StackPool::RoundUpStackSize(config.stackSize);

    auto* alloc = m_stackPool.Allocate(actual.stackSize);
    if (!alloc)
    {
        return nullptr;
    }

    auto* spawnCtx = new (alloc) Context(m_scheduled /* parent */, actual, handle, this);
    m_contexts.Push(spawnCtx);

    assert(sizeof(T) <= actual.stackSize);
    auto* launchable = new (spawnCtx->m_segment.Bottom()) T(
        spawnCtx,
        std::forward<Args>(args)...
    );

    spawnCtx->m_entry = &LaunchTrampoline<T>;
    spawnCtx->m_cleanup = &LaunchCleanup<T>;
    EnterContext(spawnCtx);
    return launchable;
}

// Type-erased submission entry that owns a lambda of type Fn. Allocated on the heap by Submit,
// freed after the cooperator spawns and executes the contained lambda.
//
template<typename Fn>
struct TypedSubmission : Cooperator::SubmissionEntry
{
    Fn m_fn;

    explicit TypedSubmission(Fn&& fn, SpawnConfiguration const& config)
    : m_fn(std::move(fn))
    {
        m_config = config;
        m_invoke = [](SubmissionEntry* self, Context* ctx)
        {
            static_cast<TypedSubmission*>(self)->m_fn(ctx);
        };
        m_destroy = [](SubmissionEntry* self)
        {
            delete static_cast<TypedSubmission*>(self);
        };
    }
};

template<typename Fn>
bool Cooperator::Submit(Fn&& fn, SpawnConfiguration const& config)
{
    if (m_shutdown.load(std::memory_order_relaxed))
    {
        return false;
    }

    using DecayFn = std::decay_t<Fn>;
    auto* entry = new TypedSubmission<DecayFn>(std::forward<Fn>(fn), config);
    PushSubmission(entry);
    WakeCooperator();
    return true;
}

template<typename Fn>
bool Cooperator::SubmitSync(Fn&& fn, SpawnConfiguration const& config)
{
    if (m_shutdown.load(std::memory_order_relaxed))
    {
        return false;
    }

    std::binary_semaphore done(0);

    using DecayFn = std::decay_t<Fn>;
    auto* entry = new TypedSubmission<DecayFn>(std::forward<Fn>(fn), config);
    entry->m_completion = &done;
    PushSubmission(entry);
    WakeCooperator();

    done.acquire();
    return true;
}

// Free-function convenience wrappers that forward to the thread-local cooperator.
//
template<typename Fn>
bool Spawn(Fn const& fn, Context::Handle* handle = nullptr)
{
    return Cooperator::thread_cooperator->Spawn(fn, handle);
}

template<typename Fn>
bool Spawn(SpawnConfiguration const& config, Fn const& fn, Context::Handle* handle = nullptr)
{
    return Cooperator::thread_cooperator->Spawn(config, fn, handle);
}

template<typename T, typename... Args>
T* Launch(SpawnConfiguration const& config, Context::Handle* handle, Args&&... args)
{
    return Cooperator::thread_cooperator->Launch<T>(config, handle, std::forward<Args>(args)...);
}

template<typename T, typename... Args>
T* Launch(SpawnConfiguration const& config, Args&&... args)
{
    return Cooperator::thread_cooperator->Launch<T>(config, std::forward<Args>(args)...);
}

template<typename T, typename... Args>
T* Launch(Context::Handle* handle, Args&&... args)
{
    return Cooperator::thread_cooperator->Launch<T>(handle, std::forward<Args>(args)...);
}

template<typename T, typename... Args>
T* Launch(Args&&... args)
{
    return Cooperator::thread_cooperator->Launch<T>(std::forward<Args>(args)...);
}

} // end namespace coop
