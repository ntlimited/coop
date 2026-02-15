#pragma once

#include <cstring>
#include <type_traits>

#include "context.h"
#include "detail/scheduler_state.h"
#include "self.h"

namespace coop
{

// Trampolines used by EnterContext's makecontext call. These are the typed entry points that know
// how to invoke the lambda (Spawn) or Launchable (Launch) stored at the segment's bottom.
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

    auto* alloc = AllocateContext(config);
    if (!alloc)
    {
        return false;
    }

    auto* spawnCtx = new (alloc) Context(m_scheduled /* parent */, config, handle, this);
    m_contexts.Push(spawnCtx);

    assert(sizeof(Fn) <= config.stackSize);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    memcpy(spawnCtx->m_segment.Bottom(), &fn, sizeof(Fn));
#pragma GCC diagnostic pop

    spawnCtx->m_entry = &SpawnTrampoline<Fn>;
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

    auto* alloc = AllocateContext(config);
    if (!alloc)
    {
        return nullptr;
    }

    auto* spawnCtx = new (alloc) Context(m_scheduled /* parent */, config, handle, this);
    m_contexts.Push(spawnCtx);

    assert(sizeof(T) <= config.stackSize);
    auto* launchable = new (spawnCtx->m_segment.Bottom()) T(
        spawnCtx,
        std::forward<Args>(args)...
    );

    spawnCtx->m_entry = &LaunchTrampoline<T>;
    EnterContext(spawnCtx);
    return launchable;
}

} // end namespace coop
