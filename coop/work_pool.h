#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include "work_deque.h"

namespace coop
{

// A unit of balanced work — a run-to-completion morsel that may execute on any cooperator. Unlike a
// continuation (single-cooperator, never migrates), a Task is shed into the pool and run by whoever
// pulls it, so it is owned cross-thread. v1 uses new/delete (malloc is thread-safe, so cross-thread
// free is correct); a per-cooperator slab with cross-thread free is a later optimization.
//
struct Task
{
    virtual void Run() = 0;
    virtual ~Task() = default;
};

template<typename Fn>
struct TaskImpl final : Task
{
    explicit TaskImpl(Fn fn) : m_fn(std::move(fn)) {}
    void Run() final { m_fn(); }
    Fn m_fn;
};

template<typename Fn>
inline Task* MakeTask(Fn&& fn)
{
    return new TaskImpl<std::decay_t<Fn>>(std::forward<Fn>(fn));
}

inline void RunTask(Task* t) { t->Run(); delete t; }

// Sharded work-stealing pool: one bounded deque per worker. Shed pushes to the caller's OWN shard
// (owner-only). Pull pops the local shard, then steals from the other shards. The deque's single-
// owner invariant is honored because Shed(w, ...) and the local pop inside Pull(w) only ever run on
// worker w's thread; other workers reach shard w solely through Steal.
//
// Generic on the payload T (a trivially-copyable pointer/handle). The general substrate uses
// T = Task* (a type-erased morsel, with MakeTask/RunTask); a caller with a stable typed work item
// (e.g. an IO pipeline that re-sheds itself each stage) uses its own pointer type to avoid a per-
// shed allocation. nullptr is the empty sentinel, so T must be nullable.
//
// This is the substrate's mechanism, kept independent of the scheduler loop for now (workers drive
// Pull explicitly). Slice 3 moves Pull into the cooperator idle path with a wake protocol.
//
template<typename T = Task*>
class WorkPool
{
    using Deque = WorkDeque<T, 8192>;

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

} // end namespace coop
