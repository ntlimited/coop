#pragma once

#include <cassert>
#include <cstddef>

#include "tricks.h"

namespace coop
{

template<typename T, typename Tag = int, const Tag N = 0>
struct EmbeddedList;

// EmbeddedListHookups works in conjunction with EmbeddedList to allow doubly linked
// lists that are safe to use within a manager's thread. The optional `Tag` templating allows multiple
// such lists in the context if we decide we want it.
//
template<typename T, typename Tag = int, const Tag N = 0>
struct EmbeddedListHookups
{
    using Hookups = EmbeddedListHookups<T, Tag, N>;
    using List = EmbeddedList<T, Tag, N>;
    using Ptr = T*;

    EmbeddedListHookups()
    : prev(nullptr)
    , next(nullptr)
    {
    }

    EmbeddedListHookups(EmbeddedListHookups const&) = delete;
    EmbeddedListHookups& operator=(EmbeddedListHookups const&) = delete;
    EmbeddedListHookups(EmbeddedListHookups&&) = delete;
    EmbeddedListHookups& operator=(EmbeddedListHookups&&) = delete;

    // These methods are only intended to be called by the list they're a member of, as otherwise
    // various guarantees may not be honored.
    //
    // TODO add some pointer tricks in debug to assert over this
    //
    friend List;

    // Replaces the current item with the other in whatever list it happens to currently live in.
    //
    void Replace(Hookups* other)
    {
        assert(!Disconnected());
        other->InsertBefore(this);
        Pop();
    }

    void Pop()
    {
        prev->SetNext(next);
        next->SetPrev(prev);
        
        next = nullptr;
        prev = nullptr;
    }

    // TODO lock down visibility
    //
    
    bool Disconnected() const
    {
        return next == nullptr;
    }

    void InsertBefore(Hookups* other)
    {
        assert(Disconnected());
        assert(other != this);
        assert(other->prev != this);

        other->prev->SetNext(this);
        SetPrev(other->prev);
        SetNext(other);
        other->SetPrev(this);
    }

    Ptr Cast()
    {
        auto* v2 = detail::ascend_cast<T, Hookups>(this);
        return v2;
    }

private:
    // Silly little helpers to make it easier to debug issues
    //
    void SetNext(Hookups* n)
    {
        assert(n);
        next = n;
    }
    void SetPrev(Hookups* p)
    {
        assert(p);
        prev = p;
    }
public:
    Hookups* prev;
    Hookups* next;
};

// An EmbeddedList has some typecasting magic and a pair of dummy list hookups to allow us to
// simplify some edge cases.
//
template<typename T, typename Tag /* = int */, const Tag N /* = 0 */>
struct EmbeddedList
{
    using Hookups = typename EmbeddedListHookups<T, Tag, N>::Hookups;
    using Ptr = Hookups::Ptr;

    struct Iterator
    {
        Hookups* hookups;

        Ptr operator*()
        {
            __builtin_prefetch(hookups->next, 0, 0);
            return hookups->Cast();
        }

        Ptr operator->()
        {
            return hookups->Cast();
        }

        Iterator& operator++()
        {
            hookups = hookups->next;
            return *this;
        }

        bool operator==(Iterator const& other) const
        {
            return hookups == other.hookups;
        }
    };

    EmbeddedList()
    {
        sentinel.next = &sentinel;
        sentinel.prev = &sentinel;
    }

    Iterator begin()
    {
        return Iterator(sentinel.next);
    }

    Iterator end()
    {
        return Iterator(&sentinel);
    }

    void Push(Hookups* item)
    {
        assert(item->Disconnected());
        auto* last = sentinel.prev;
        last->next = item;
        item->prev = last;
        item->next = &sentinel;
        sentinel.prev = item;
    }

    Ptr Pop()
    {
        auto* first = sentinel.next;
        if (first == &sentinel) [[unlikely]]
        {
            return nullptr;
        }

        auto* second = first->next;
        sentinel.next = second;
        second->prev = &sentinel;

#ifndef NDEBUG
        first->next = nullptr;
        first->prev = nullptr;
#endif

        return first->Cast();
    }

    Ptr Peek()
    {
        return sentinel.next->Cast();
    }

    size_t Size() const
    {
        size_t n = 0;
        auto* at = sentinel.next;
        while (at != &sentinel)
        {
            at = at->next;
            n++;
        }
        return n;
    }

    bool IsEmpty() const
    {
        return sentinel.next == &sentinel;
    }

    void Remove(Hookups* h)
    {
        h->Pop();
    }

    void SanityCheck(Hookups* checkFor = nullptr)
    {
        bool found = false;
        auto* at = sentinel.next;
        int n = 0;
        while (at != &sentinel)
        {
            if (at == checkFor)
            {
                found = true;
            }
            at = at->next;
            n++;
        }
        assert(found || !checkFor);

        at = sentinel.prev;
        while (at != &sentinel)
        {
            at = at->prev;
            if (0 > n--)
            {
                assert(false);
            }
        }
    }

    // NOTE this is a dangerous as hell contract as yielding while within this method can lead to
    // completely broken contracts; the nodes we are moving over may be unhooked from this list and
    // rehooked into completely different ones.
    //
    template<typename Fn>
    void Visit(Fn const& fn)
    {
        auto* at = sentinel.next;
        while (at != &sentinel)
        {
            auto* next = at->next;
            __builtin_prefetch(next, 0, 0);
            auto* casted = at->Cast();
            at = next;
            if (!fn(casted))
            {
                return;
            }
        }
    }

  private:
    Hookups sentinel;
};

} // end namespace coop
