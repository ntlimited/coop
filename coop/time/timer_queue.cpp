#include "timer_queue.h"

namespace coop
{

namespace time
{

TimerNode* TimerQueue::MergePairs(TimerNode* first)
{
    if (!first)
    {
        return nullptr;
    }

    // Pass 1: meld consecutive pairs left to right, pushing each merged result onto a temporary
    // stack threaded through m_next. A detached subtree's m_next/m_prev are null, so reusing m_next
    // as the stack link here is safe.
    //
    TimerNode* stack = nullptr;
    TimerNode* cur   = first;
    while (cur)
    {
        TimerNode* a = cur;
        TimerNode* b = a->m_next;
        if (b)
        {
            cur = b->m_next;
            a->m_next = a->m_prev = nullptr;
            b->m_next = b->m_prev = nullptr;
            TimerNode* m = Meld(a, b);
            m->m_next = stack;
            stack = m;
        }
        else
        {
            cur = nullptr;
            a->m_next = a->m_prev = nullptr;
            a->m_next = stack;
            stack = a;
        }
    }

    // Pass 2: fold the stack into a single tree.
    //
    TimerNode* result = nullptr;
    while (stack)
    {
        TimerNode* next = stack->m_next;
        stack->m_next = nullptr;
        stack->m_prev = nullptr;
        result = Meld(result, stack);
        stack = next;
    }
    return result;
}

void TimerQueue::Remove(TimerNode* node)
{
    if (!node->m_linked)
    {
        return;
    }
    node->m_linked = false;

    if (node == m_root)
    {
        m_root = MergePairs(node->m_child);
        if (m_root)
        {
            m_root->m_prev = nullptr;
        }
    }
    else
    {
        // Unlink the node from its parent's child list. m_prev is the parent when the node is the
        // leftmost child, otherwise a left sibling -- distinguished by whether m_prev's child is us.
        //
        if (node->m_prev->m_child == node)
        {
            node->m_prev->m_child = node->m_next;
        }
        else
        {
            node->m_prev->m_next = node->m_next;
        }
        if (node->m_next)
        {
            node->m_next->m_prev = node->m_prev;
        }

        // Merge the removed node's own children back into the heap.
        //
        TimerNode* sub = MergePairs(node->m_child);
        if (sub)
        {
            sub->m_prev = nullptr;
        }
        m_root = Meld(m_root, sub);
        if (m_root)
        {
            m_root->m_prev = nullptr;
        }
    }

    node->m_child = node->m_next = node->m_prev = nullptr;
}

bool TimerQueue::Validate(const TimerNode* n)
{
    if (!n)
    {
        return true;
    }
    for (const TimerNode* c = n->m_child; c; c = c->m_next)
    {
        if (c->m_deadlineUs < n->m_deadlineUs)
        {
            return false;
        }
        if (!Validate(c))
        {
            return false;
        }
    }
    return true;
}

} // end namespace time
} // end namespace coop
