#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

namespace coop
{

struct Cooperator;

// CooperatorVar<T> provides per-cooperator typed storage with the same declaration ergonomics
// as ContextVar<T>, but with lifetime tied to the cooperator rather than a context.
//
// Storage lives at a fixed offset within an inline byte array in the Cooperator struct. Offsets
// are assigned at static initialization time. Access is: thread-local cooperator pointer +
// constant offset + dereference. One TLS load, one ADD.
//
// Construction is eager: all registered CooperatorVars are constructed in the cooperator's
// constructor and destructed in its destructor. This avoids a per-access initialization branch.
//
// CooperatorVar constructors (T()) must not depend on a live cooperator — the cooperator is
// mid-construction when ConstructAll() runs. Use an explicit Init() method for post-construction
// setup (e.g., setting pointers after subsystem initialization).
//
// CooperatorVar destructors run in the cooperator's destructor, before the cooperator's own
// members are destroyed. This ensures cooperator-var objects can safely reference cooperator
// infrastructure during teardown.
//
// Usage:
//
//   // file scope
//   static CooperatorVar<CooperatorDBState> s_dbState;
//
//   // in any cooperating code (uses thread_cooperator)
//   s_dbState->fragSource.Acquire();
//
//   // with explicit cooperator
//   s_dbState.Get(&cooperator)->fragSource.Acquire();
//

namespace detail
{

struct CooperatorVarRegistry
{
    struct Entry
    {
        size_t offset;
        size_t size;
        size_t align;
        void (*construct)(void*);
        void (*destruct)(void*);
    };

    static CooperatorVarRegistry& Instance()
    {
        static CooperatorVarRegistry s_instance;
        return s_instance;
    }

    // Register a new CooperatorVar slot. Returns the assigned byte offset from storage base.
    // Called during static initialization — must not be called after any Cooperator has been
    // created.
    //
    size_t Register(size_t size, size_t align, void (*ctor)(void*), void (*dtor)(void*))
    {
        assert(!m_frozen && "CooperatorVar registered after first Cooperator creation");
        assert(m_count < MAX_VARS);

        // Align the current watermark.
        //
        size_t offset = (m_totalSize + align - 1) & ~(align - 1);

        m_entries[m_count++] = Entry{ offset, size, align, ctor, dtor };
        m_totalSize = offset + size;

        return offset;
    }

    // Total reserved bytes in the cooperator's local storage.
    //
    size_t TotalSize() const
    {
        return (m_totalSize + 15) & ~size_t(15);
    }

    // Construct all registered CooperatorVars in a cooperator's local storage.
    //
    void ConstructAll(void* storage)
    {
        m_frozen = true;
        for (size_t i = 0; i < m_count; i++)
        {
            m_entries[i].construct(static_cast<char*>(storage) + m_entries[i].offset);
        }
    }

    // Destruct all registered CooperatorVars. Reverse order for symmetry with construction.
    //
    void DestructAll(void* storage)
    {
        for (size_t i = m_count; i > 0; i--)
        {
            m_entries[i - 1].destruct(
                static_cast<char*>(storage) + m_entries[i - 1].offset);
        }
    }

    size_t Count() const { return m_count; }

private:
    CooperatorVarRegistry() = default;

    static constexpr size_t MAX_VARS = 16;

    Entry   m_entries[MAX_VARS] = {};
    size_t  m_count{0};
    size_t  m_totalSize{0};
    bool    m_frozen{false};
};

} // end namespace coop::detail


template<typename T>
struct CooperatorVar
{
    CooperatorVar()
    {
        m_offset = detail::CooperatorVarRegistry::Instance().Register(
            sizeof(T), alignof(T),
            [](void* p) { new (p) T(); },
            [](void* p) { static_cast<T*>(p)->~T(); }
        );
    }

    // Access via the thread-local current cooperator.
    //
    T* operator->()             { return Get(); }
    const T* operator->() const { return Get(); }
    T& operator*()              { return *Get(); }
    const T& operator*() const  { return *Get(); }

    T* Get();
    const T* Get() const;

    // Access with an explicit cooperator.
    //
    T* Get(Cooperator* coop);
    const T* Get(Cooperator* coop) const;

private:
    size_t m_offset;
};

} // end namespace coop
