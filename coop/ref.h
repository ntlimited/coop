#pragma once

#include <type_traits>
#include <cassert>

#include "coordinator.h"
#include "coordinator_extension.h"
#include "self.h"
#include "tricks.h"

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
    // TODO this is a very obvious place to build some kind of context-based gc where we could
    // attach hazard lists, but in theory there shouldn't be much else natively that requires
    // this kind of treatment; contexts are special as even conceptually they have something of
    // a zombie state
    //
    Reffed(Context* ctx)
    : m_refs(1)
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
            CoordinatorExtension().Shutdown(&m_zeroSignal, ::coop::Self());
            return;
        }
        assert(m_refs > 0);
    }

    T* Cast()
    {
        return detail::ascend_cast<T, Reffed>(this);
    }

  protected:
    int m_refs;
    Coordinator m_zeroSignal;
};

} // end namespace coop
