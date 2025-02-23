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

    EmbeddedListHookups()
    : prev(nullptr)
    , next(nullptr)
    {
    }

  protected:
    // These methods are only intended to be called by the list they're a member of, as otherwise
    // various guarantees may not be honored.
    //
    // TODO add some pointer tricks in debug to assert over this
    //
    friend List;

    void Pop()
    {
        prev->next = next;
        next->prev = prev;
    }

    void InsertBefore(Hookups* other)
    {
        other->prev->next = this;
        this->prev = other->prev;
        other->prev = this;
        this->next = other;
    }

private:
    Hookups* prev;
    Hookups* next;
};

template<typename T, typename Tag /* = int */, const Tag N /* = 0 */>
struct EmbeddedList
{
    using Hookups = typename EmbeddedListHookups<T, Tag, N>::Hookups;

    EmbeddedList()
    {
        head.prev = nullptr;
        head.next = &tail;
        tail.prev = &head;
        tail.next = nullptr;
        size = 0;
    }

    void Push(Hookups* item)
    {
        item->InsertBefore(&tail);
        size++;
    }

    bool Pop(T*& out)
    {
        if (IsEmpty())
        {
            return false;
        }

        out = Cast(head.next);
        head.next->Pop();
        size--;
        return true;
    }

    T* Peek()
    {
        return Cast(head.next);
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
    }

    template<typename Fn>
    void Visit(Fn const& fn)
    {
        auto* at = head.next;
        while (at != &tail)
        {
            // Iterate forward now incase it gets removed and relinked somewhere else
            //
            auto* casted = Cast(at);
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

    static T* Cast(Hookups* h)
    {
        auto* v2 = detail::ascend_cast<T, Hookups>(h);
        return v2;
    }
};

} // end namespace coop
