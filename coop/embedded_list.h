#pragma once

#include <cassert>
#include <cstddef>
#include <stdio.h>

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
        return next == prev;
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
        head.prev = nullptr;
        head.next = &tail;
        tail.prev = &head;
        tail.next = nullptr;
        size = 0;
    }

    Iterator begin()
    {
        return Iterator(head.next);
    }

    Iterator end()
    {
        return Iterator(&tail);
    }

    void Push(Hookups* item)
    {
        item->InsertBefore(&tail);
        size++;
    }

    bool Pop(Ptr& out)
    {
        if (IsEmpty())
        {
            return false;
        }

        out = head.next->Cast();
        head.next->Pop();
        size--;
        return true;
    }

    Ptr Peek()
    {
        return head.next->Cast();
    }

    size_t Size() const
    {
        return size;
    }

    bool IsEmpty() const
    {
        return head.next == &tail;
    }

    void Remove(Hookups* h)
    {
        h->Pop();
        size--;
    }

    void SanityCheck(Hookups* checkFor = nullptr)
    {
        bool found = false;
        auto* at = head.next;
        int n = 0;
        while (at != &tail)
        {
            if (at == checkFor)
            {
                found = true;
            }
            at = at->next;
            n++;
        }
        assert(found || !checkFor);
        assert(n == size);

        at = tail.prev;
        while (at != &head)
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
        auto* at = head.next;
        while (at != &tail)
        {
            // Iterate forward now incase it gets removed and relinked somewhere else
            //
            auto* casted = at->Cast();
            at = at->next;
            
            if (!fn(casted))
            {
                return;
            }
        }
    }

  private:
    Hookups head;
    Hookups tail;
    size_t size;
};

} // end namespace coop
