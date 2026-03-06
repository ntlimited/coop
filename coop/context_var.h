#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

#include "self.h"
#include "context.h"

namespace coop
{

// ContextVar<T> is TLS wearing a hat. It provides per-context typed storage with the same
// declaration ergonomics as thread_local — declare at file scope, access cheaply — but with
// lifetime tied to the context rather than the OS thread.
//
// Storage lives at a fixed offset from the context's segment base, below the Launchable and
// bump heap. The offset is assigned at static initialization time and never changes. Access
// is: thread-local cooperator → scheduled context → segment base + constant offset. The
// compiler can CSE the first two loads across multiple ContextVar accesses in the same function.
//
// Construction is eager: all registered ContextVars are constructed for every context at
// creation time and destructed at teardown. This avoids a per-access initialization branch.
// Keep ContextVar for pervasive per-context state (epoch participation, transaction state,
// perf counters) — not for niche per-task concerns that belong in explicit plumbing.
//
// ContextVar destructors must not do cooperative work (no yields, no blocks). They run after
// the task and its Launchable have been destructed but before ~Context().
//
// Usage:
//
//   // file scope
//   static ContextVar<EpochState> s_epoch;
//
//   // in any cooperating code
//   s_epoch->currentEpoch = mgr.Enter();
//
//   // with explicit context
//   s_epoch.Get(ctx)->currentEpoch = ...;
//

namespace detail
{

struct ContextVarRegistry
{
    struct Entry
    {
        size_t offset;
        size_t size;
        size_t align;
        void (*construct)(void*);
        void (*destruct)(void*);
    };

    static ContextVarRegistry& Instance()
    {
        static ContextVarRegistry s_instance;
        return s_instance;
    }

    // Register a new ContextVar slot. Returns the assigned byte offset from segment base.
    // Called during static initialization — must not be called after any Context has been created.
    //
    size_t Register(size_t size, size_t align, void (*ctor)(void*), void (*dtor)(void*))
    {
        assert(!m_frozen && "ContextVar registered after first Context creation");
        assert(m_count < MAX_VARS);

        // Align the current watermark
        //
        size_t offset = (m_totalSize + align - 1) & ~(align - 1);

        m_entries[m_count++] = Entry{ offset, size, align, ctor, dtor };
        m_totalSize = offset + size;

        return offset;
    }

    // Total reserved bytes at the base of every context segment. Called by Spawn/Launch to
    // position the Launchable above the ContextVar region.
    //
    size_t TotalSize() const
    {
        // Align up to 16 so the Launchable placement starts on a clean boundary
        //
        return (m_totalSize + 15) & ~size_t(15);
    }

    // Construct all registered ContextVars in a freshly-created context's segment.
    //
    void ConstructAll(void* segmentBase)
    {
        m_frozen = true;
        for (size_t i = 0; i < m_count; i++)
        {
            m_entries[i].construct(static_cast<char*>(segmentBase) + m_entries[i].offset);
        }
    }

    // Destruct all registered ContextVars. Called during context teardown, after the Launchable
    // destructor but before ~Context(). Reverse order for symmetry.
    //
    void DestructAll(void* segmentBase)
    {
        for (size_t i = m_count; i > 0; i--)
        {
            m_entries[i - 1].destruct(
                static_cast<char*>(segmentBase) + m_entries[i - 1].offset);
        }
    }

    size_t Count() const { return m_count; }

private:
    ContextVarRegistry() = default;

    static constexpr size_t MAX_VARS = 32;

    Entry   m_entries[MAX_VARS] = {};
    size_t  m_count{0};
    size_t  m_totalSize{0};
    bool    m_frozen{false};
};

} // end namespace coop::detail


// ContextVarTotalSize is the public accessor for use by Spawn/Launch and trampolines.
//
inline size_t ContextVarTotalSize()
{
    return detail::ContextVarRegistry::Instance().TotalSize();
}


template<typename T>
struct ContextVar
{
    ContextVar()
    {
        m_offset = detail::ContextVarRegistry::Instance().Register(
            sizeof(T), alignof(T),
            [](void* p) { new (p) T(); },
            [](void* p) { static_cast<T*>(p)->~T(); }
        );
    }

    // Access via the thread-local current context
    //
    T* operator->()             { return Get(); }
    const T* operator->() const { return Get(); }
    T& operator*()              { return *Get(); }
    const T& operator*() const  { return *Get(); }

    T* Get()
    {
        return GetFrom(Self());
    }

    const T* Get() const
    {
        return GetFrom(Self());
    }

    // Access with an explicit context
    //
    T* Get(Context* ctx)
    {
        return GetFrom(ctx);
    }

    const T* Get(Context* ctx) const
    {
        return GetFrom(ctx);
    }

private:
    T* GetFrom(Context* ctx) const
    {
        return reinterpret_cast<T*>(
            static_cast<char*>(ctx->m_segment.Bottom()) + m_offset);
    }

    size_t m_offset;
};

} // end namespace coop
