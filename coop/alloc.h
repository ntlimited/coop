#pragma once

#include <cstddef>
#include <new>
#include <type_traits>

#include "context.h"
#include "self.h"
#include "detail/bump.h"

namespace coop
{

// RAII bump allocation for typed objects. Constructs T with optional trailing bytes (for flexible
// array patterns like `char m_buf[0]`) and destructs + de-bumps on scope exit. Strictly LIFO
// with respect to other bump allocations on the same context.
//
// Two construction modes:
//
//   Direct:    Alloc<Concrete>(ctx, extra, args...)
//              ctx->Allocate<Concrete>(extra, args...)
//
//   Interface: Alloc<Interface>::From<Concrete>(ctx, extra, args...)
//              Constructs Concrete, exposes as Interface*. The interface must have a virtual
//              destructor. For dynamic type selection with a pure-virtual API boundary.
//
template<typename T>
struct Alloc
{
    Alloc() : m_ptr(nullptr), m_ctx(nullptr) {}

    // Direct construction: T is the concrete type.
    //
    template<typename... Args>
    Alloc(Context* ctx, size_t extra, Args&&... args)
    : m_ctx(ctx)
    {
        void* mem = detail::BumpAlloc(ctx, sizeof(T) + extra, alignof(T));
        m_ptr = new (mem) T(std::forward<Args>(args)...);
    }

    // Interface construction: allocate Concrete, expose as T (the interface type).
    // Usage: auto conn = Alloc<ConnectionBase>::From<Connection<PT>>(ctx, extra, args...);
    //
    template<typename Concrete, typename... Args>
    static Alloc From(Context* ctx, size_t extra, Args&&... args)
    {
        static_assert(std::is_base_of_v<T, Concrete>,
            "Concrete must derive from the interface type T");
        static_assert(std::has_virtual_destructor_v<T>,
            "Interface type must have a virtual destructor for From<Concrete> to work");

        void* mem = detail::BumpAlloc(ctx, sizeof(Concrete) + extra, alignof(Concrete));
        new (mem) Concrete(std::forward<Args>(args)...);
        T* ptr = static_cast<T*>(static_cast<Concrete*>(mem));
        return Alloc(ctx, ptr);
    }

    ~Alloc()
    {
        if (m_ptr)
        {
            m_ptr->~T();
            detail::BumpFree(m_ctx, m_ptr);
        }
    }

    Alloc(Alloc const&) = delete;
    Alloc& operator=(Alloc const&) = delete;
    Alloc(Alloc&&) = delete;
    Alloc& operator=(Alloc&&) = delete;

    T* operator->() { return m_ptr; }
    const T* operator->() const { return m_ptr; }
    T& operator*() { return *m_ptr; }
    const T& operator*() const { return *m_ptr; }
    T* get() { return m_ptr; }
    const T* get() const { return m_ptr; }

  private:
    Alloc(Context* ctx, T* ptr) : m_ptr(ptr), m_ctx(ctx) {}

    T*        m_ptr;
    Context*  m_ctx;
};

// RAII bump allocation for raw byte buffers. No construction/destruction — just reserves space
// from the context's bump heap and frees it (LIFO) on scope exit.
//
// Usage: auto buf = ctx->AllocateBuffer(4096);
//        memcpy(buf.data(), src, buf.size());
//
struct AllocBuffer
{
    AllocBuffer() : m_data(nullptr), m_size(0), m_ctx(nullptr) {}

    AllocBuffer(Context* ctx, size_t size)
    : m_ctx(ctx)
    , m_size(size)
    {
        m_data = static_cast<char*>(detail::BumpAlloc(ctx, size));
    }

    ~AllocBuffer()
    {
        if (m_data) detail::BumpFree(m_ctx, m_data);
    }

    AllocBuffer(AllocBuffer const&) = delete;
    AllocBuffer& operator=(AllocBuffer const&) = delete;
    AllocBuffer(AllocBuffer&&) = delete;
    AllocBuffer& operator=(AllocBuffer&&) = delete;

    char* data() { return m_data; }
    const char* data() const { return m_data; }
    size_t size() const { return m_size; }
    char& operator[](size_t i) { return m_data[i]; }
    const char& operator[](size_t i) const { return m_data[i]; }

  private:
    char*     m_data;
    size_t    m_size;
    Context*  m_ctx;
};

// Context::Allocate — method form. Returns Alloc<T> by value via C++17 guaranteed copy elision.
//
template<typename T, typename... Args>
Alloc<T> Context::Allocate(size_t extra, Args&&... args)
{
    return Alloc<T>(this, extra, std::forward<Args>(args)...);
}

// Context::AllocateBuffer — method form for raw byte buffers.
//
inline AllocBuffer Context::AllocateBuffer(size_t size)
{
    return AllocBuffer(this, size);
}

// Free-function forms that use the thread-local context.
//
template<typename T, typename... Args>
Alloc<T> Allocate(size_t extra, Args&&... args)
{
    return Self()->Allocate<T>(extra, std::forward<Args>(args)...);
}

inline AllocBuffer AllocateBuffer(size_t size)
{
    return Self()->AllocateBuffer(size);
}

} // end namespace coop
