#pragma once

#include <cstddef>
#include <memory>

#include "deque.h"

namespace coop
{
namespace work
{
namespace detail
{

// Sharded work-stealing pool: one bounded Deque per worker. Shed pushes to the caller's OWN shard
// (owner-only). Pull pops the local shard, then steals from the other shards. The Deque's single-
// owner invariant is honored because Shed(w, ...) and the local pop inside Pull(w) only ever run on
// worker w's thread; other workers reach shard w solely through Steal.
//
// Generic on the payload T (a trivially-copyable nullable pointer). The Grid uses T = Erg*; a caller
// with a stable typed work item (an IO pipeline that re-sheds itself each stage) can use its own
// pointer to avoid a per-shed allocation. This is the steal mechanism; the Grid layers the
// cooperator participation, stealers, and the Shed verb on top.
//
template<typename T>
class Shards
{
    using Deque = detail::Deque<T, 8192>;

  public:
    void Init(int n) { m_n = n; m_shards.reset(new Deque[n]); }
    int  Workers() const { return m_n; }

    // Owner of shard w only. Returns false if the shard is full (caller overflows out of band).
    //
    bool Shed(int w, T t) { return m_shards[w].PushBottom(t); }

    // Worker w: take local work (LIFO), else steal from another shard (FIFO). nullptr if all empty.
    //
    T Pull(int w)
    {
        T t;
        if (m_shards[w].PopBottom(t)) return t;
        for (int i = 1; i < m_n; i++)
        {
            if (m_shards[(w + i) % m_n].Steal(t)) return t;
        }
        return nullptr;
    }

  private:
    std::unique_ptr<Deque[]> m_shards;
    int m_n = 0;
};

} // end namespace detail
} // end namespace work
} // end namespace coop
