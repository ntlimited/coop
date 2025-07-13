#pragma once

namespace coop
{

template<typename T, size_t C>
struct FixedList
{
    static_assert(0 == (C & (C-1)), "C must be 1<<X");

    FixedList()
    {
        m_head = 0;
        m_tail = 0;
    }

    bool Push(T t)
    {
        auto newTail = (m_tail + 1) & (C - 1);
        if (newTail == m_head)
        {
            return false;
        }
        m_slots[m_tail] = t;
        m_tail = newTail;
        return true;
    }

    bool Pop(T& out)
    {
        if (m_head == m_tail)
        {
            return false;
        }
        out = m_slots[m_head];
        m_head = (m_head + 1) & (C - 1);
        return true;
    }

    bool IsEmpty() const
    {
        return m_tail == m_head;
    }

    bool CanPush() const
    {
        return m_tail != ((m_head - 1) & (C - 1));
    }

    template<typename Fn>
    void ForEach(Fn const& fn)
    {
        auto head = m_head;
        while (head != m_tail)
        {
            if (fn(m_slots[head]))
            {
                head = (head + 1) & (C - 1);
            }
            else
            {
                m_slots[m_head]  = m_slots[m_tail];
                m_tail = (m_tail - 1) & (C - 1);
            }
        }
    }

    void Remove(T toRemove)
    {
        ForEach([toRemove](T other) -> bool
        {
            return other != toRemove;
        });
    }

    T m_slots[C];
    size_t m_head;
    size_t m_tail;
};

} // end namespace coop
