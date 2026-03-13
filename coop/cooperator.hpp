#pragma once

#include <type_traits>

#include "context.h"
#include "context_var.h"
#include "detail/scheduler_state.h"
#include "self.h"

namespace coop
{

// Typed entry points that know how to invoke the lambda (Spawn) or Launchable (Launch) stored
// at the segment's bottom, past the ContextVar region. Called from CoopContextEntry via
// ctx->m_entry.
//
template<typename Fn>
void SpawnTrampoline(Context* ctx)
{
    auto* fn = reinterpret_cast<Fn*>(
        static_cast<char*>(ctx->m_segment.Bottom()) + ContextVarTotalSize());
    (*fn)(ctx);
}

template<typename T>
void LaunchTrampoline(Context* ctx)
{
    auto* obj = reinterpret_cast<T*>(
        static_cast<char*>(ctx->m_segment.Bottom()) + ContextVarTotalSize());
    obj->Launch();
}

template<typename T>
void LaunchCleanup(Context* ctx)
{
    auto* obj = reinterpret_cast<T*>(
        static_cast<char*>(ctx->m_segment.Bottom()) + ContextVarTotalSize());
    obj->~T();
}

template<typename Fn>
bool Cooperator::Spawn(Fn const& fn, Context::Handle* handle /* = nullptr */)
{
    // Inherit the parent context's config when no explicit config is given.
    // This ensures child contexts get the same stack size as their parent,
    // preventing small-stack children from being allocated at virtual addresses
    // that overlap with a parent's freed large-stack range.
    //
    if (m_scheduled)
    {
        SpawnConfiguration inherited = {
            .priority = m_scheduled->m_priority,
            .stackSize = m_scheduled->m_segment.Size()
        };
        return Spawn(inherited, fn, handle);
    }
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
    assert(actual.stackSize >= sizeof(Context));

    auto* alloc = m_stackPool.Allocate(actual.stackSize);
    if (!alloc)
    {
        return false;
    }

    auto* spawnCtx = new (alloc) Context(m_scheduled /* parent */, actual, handle, this);
    m_contexts.Push(spawnCtx);

    size_t varSize = ContextVarTotalSize();
    void* launchBase = static_cast<char*>(spawnCtx->m_segment.Bottom()) + varSize;

    assert(varSize + sizeof(Fn) <= actual.stackSize);
    detail::ContextVarRegistry::Instance().ConstructAll(spawnCtx->m_segment.Bottom());
    new (launchBase) Fn(fn);

    uintptr_t heapStart = reinterpret_cast<uintptr_t>(launchBase) + sizeof(Fn);
    heapStart = (heapStart + 15) & ~uintptr_t(15);
    spawnCtx->m_heapTop = reinterpret_cast<void*>(heapStart);

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
    assert(actual.stackSize >= sizeof(Context));

    auto* alloc = m_stackPool.Allocate(actual.stackSize);
    if (!alloc)
    {
        return nullptr;
    }

    auto* spawnCtx = new (alloc) Context(m_scheduled /* parent */, actual, handle, this);
    m_contexts.Push(spawnCtx);

    size_t varSize = ContextVarTotalSize();
    void* launchBase = static_cast<char*>(spawnCtx->m_segment.Bottom()) + varSize;

    assert(varSize + sizeof(T) <= actual.stackSize);
    detail::ContextVarRegistry::Instance().ConstructAll(spawnCtx->m_segment.Bottom());
    auto* launchable = new (launchBase) T(
        spawnCtx,
        std::forward<Args>(args)...
    );

    uintptr_t heapStart = reinterpret_cast<uintptr_t>(launchBase) + sizeof(T);
    heapStart = (heapStart + 15) & ~uintptr_t(15);
    spawnCtx->m_heapTop = reinterpret_cast<void*>(heapStart);

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
