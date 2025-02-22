#pragma once

#include <cassert>

#include "coordinator.h"
#include "coordinator_extension.h"

namespace coop
{

template<typename T>
struct Ref
{
    Ref(Ref const&) = delete;
    Ref(Ref&& r)
    {
        refers = r.refers;
        r.refers = nullptr;
    }
    
    Ref(Ref& r)
    {
        refers = r.refers;
        refers->AddRef();
    }
    
    Ref(T* t)
    {
        refers = t;
        refers->AddRef();
    }

    ~Ref()
    {
        if (refers)
        {
            refers->RemoveRef();
        }
    }

    T* operator->()
    {
        return refers;
    }

  private:
    T* refers;
};

template<typename T>
struct Reffed
{
    // A Reffed type must be tied to the lifetime of a Context, which also works out with it being
    // used for the lifetime of a Context.
    //
    Reffed(Context* ctx)
    : m_refs(0)
    , m_zeroSignal(ctx)
    {
    }

    Ref<T> TakeRef()
    {
        return Ref<T>(Cast());
    }

    void AddRef()
    {
        m_refs++;
    }

    void RemoveRef()
    {
        if (!--m_refs)
        {
            // I don't feel great about this; is it an excuse to use Self?
            //
            m_zeroSignal.Release(nullptr);
            return;
        }
        assert(m_refs > 0);
    }

    T* Cast()
    {
        return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) - RawCastOffset());
    }

    static constexpr ptrdiff_t RawCastOffset()
    {
        return reinterpret_cast<ptrdiff_t>((Reffed*)((T*)0x1000)) - (ptrdiff_t)0x1000;
    }

  protected:
    int m_refs;
    Coordinator m_zeroSignal;
};

} // end namespace coop
