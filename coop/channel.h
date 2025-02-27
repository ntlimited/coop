#pragma once

#include "coordinator.h"

// This was intended as the second-level building block for coordination and to stress the system
// a bit. It's intended to be a semaphore, a basic
//
// - unidirectional channel would be fun. I think virtual inheritance could achieve this.
//

namespace coop
{

// BaseChannel is currently just a way to add a common ancestor to the template specialization.
//
// There's a pretty crazy amount of refactoring in general here around the coordination behaviors
// that are 90% the same repeated 8x or so in total, potentially, that it would be nice to see.
//
struct BaseChannel
{
};

// Channel is basically wholesale copied from golang in terms of the semantics, interface, and
// patterns it allows.
//
// With respect to implementation, a channel mantains a circular queue and a pair of coordinators
// that gate access to reads and writes respectively. The coordinators allow forming waiter queues
// for these operations. The logic is:
//
// (1)  Check if the operation would block; if not, skip to step 6
// (2)  Acquire the coordinator for the operation
// (3)  Perform the operation
// (4)  Release the coordinator
// (5)  Skip to step 8
// (6)  Perform the operation
// (7)  If the opposing coordinator is held, release it
//
// There is also a shutdown state that prevents using the channel further for sending, including
// releasing any receivers with failure.
//
template<typename T>
struct Channel : BaseChannel
{
    Channel(Channel const& other) = delete;
    Channel(Channel&&) = delete;

    Channel(T* buffer, size_t capacity)
    : m_buffer(buffer)
    , m_head(0)
    , m_tail(0)
    , m_capacity(capacity)
    , m_shutdown(false)
    {
    }

    virtual ~Channel()
    {
    }

    // Shutdown the channel
    //
    void Shutdown(Context* ctx)
    {
        m_shutdown = true;
        if (m_send.IsHeld())
        {
            m_send.Release(ctx);
        }

        if (m_recv.IsHeld())
        {
            m_recv.Release(ctx);
        }
    }

    // Push adds the given item to the channel, returning failure if the channel was shutdown
    // before the push completed.
    //
    bool Send(Context* ctx, T value)
    {
        if (TrySend(ctx, value))
        {
            return true;
        }

        // Blocking codepath: note that IsShutdown() can get flipped while we were coordinating
        //
        m_send.Acquire(ctx);
        if (IsShutdown())
        {
            m_send.Release(ctx);
            return false;
        }

        SendImpl(value);
        
        // If there is space, let go of the coordinator so that the preconditions still stand. This
        // in theory wouldn't happen today but if we change the unblock mechanics to not just
        // chain immediately it's very possible for multiple Recvs before this wakes up holding the
        // coordinator.
        //
        if (!IsFull())
        {
            m_send.Release(ctx);
        }

        if (m_recv.IsHeld() && !IsEmpty())
        {
            m_recv.Release(ctx);
        }
    }

    // Mirror image of the above but we handle shutdown differently
    //
    bool Recv(Context* ctx, T& value /* out */)
    {
        if (TryRecv(ctx, value))
        {
            return true;
        }

        m_recv.Acquire(ctx);

        // This is possible in the shutdown case
        //
        if (!RecvImpl(value))
        {
            assert(IsShutdown());
            m_recv.Release(ctx);
            return false;
        }

        // If we are shutdown, no more things can come through so signal anyone else who is waiting
        // on the coordinator; alternatively, if there is more to recv, then let go of it for the
        // next coordinatee.
        //
        if ((Shutdown() && IsEmpty()) || !IsEmpty())
        {
            m_recv.Release(ctx);
        }

        if (m_send.IsHeld() && !IsFull())
        {
            m_send.Release(ctx);
        }

        return true;
    }

    bool TrySend(Context* ctx, T value)
    {
        if (IsShutdown() || IsFull())
        {
            return false;
        }

        SendImpl(value);
        
        if (IsFull())
        {
            m_send.Acquire(ctx);
        }

        // Note that this is not the same as the non-try case
        //
        if (m_recv.IsHeld())
        {
            m_recv.Release(ctx);
        }
        return true;
    }

    bool TryRecv(Context* ctx, T& value /* out */)
    {
        if (IsEmpty())
        {
            return false;
        }

        RecvImpl(ctx, value);

        if (IsEmpty())
        {
            m_recv.Acquire(ctx);
        }

        if (m_send.IsHeld())
        {
            m_send.Release(ctx);
        }

        return true;
    }

    bool IsShutdown() const
    {
        return m_shutdown;
    }

  protected:
    // _Impl methods are both step 1 (check if it is possible to do the operation nonblockingly)
    // and step 4 (just do the operation)
    //

    bool SendImpl(Context* ctx, T value)
    {
        if (IsFull())
        {
            return false;
        }
        m_buffer[m_tail++] = value;
        if (m_tail == m_capacity)
        {
            m_tail = 0;
        }
        return true;
    }

    bool RecvImpl(T& value)
    {
        if (IsEmpty())
        {
            return false;
        }

        value = m_buffer[m_head++];
        if (m_head == m_capacity)
        {
            m_head = 0;
        }
        return true;
    }

    bool IsFull() const
    {
        auto next = (m_tail + 1) % m_capacity;
        return next ==  m_head;
    }

    bool IsEmpty() const
    {
        return m_head == m_tail;
    }

  private:
    Coordinator m_send;
    Coordinator m_recv;

    T* m_buffer;
    size_t m_head;
    size_t m_tail;
    size_t m_capacity;
    bool m_shutdown;
};

template<>
struct Channel<void> : BaseChannel
{
    Channel(int capacity, int used = 0)
    : m_size(used)
    , m_capacity(capacity)
    , m_shutdown(false)
    {
        assert(m_capacity < m_size);
    }

    bool TrySend(Context* ctx)
    {
        if (IsShutdown() || IsFull())
        {
            return false;
        }

        SendImpl();

        if (IsFull())
        {
            m_send.Acquire(ctx);
        }

        if (m_recv.IsHeld())
        {
            m_recv.Release(ctx);
        }

        return true;
    }

    bool TryRecv(Context* ctx)
    {
        if (IsEmpty())
        {   
            return false;
        }

        RecvImpl();
        
        if (IsEmpty())
        {
            m_recv.Acquire(ctx);
        }

        if (m_send.IsHeld())
        {
            m_send.Release(ctx);
        }
        return true;
    }

    bool Send(Context* ctx)
    {
        if (TrySend(ctx))
        {
            return true;
        }
        m_send.Acquire(ctx);
        if (IsShutdown())
        {
            m_send.Release(ctx);
            return false;
        }

        SendImpl();

        if (!IsFull())
        {
            m_send.Release(ctx);
        }

        if (m_recv.IsHeld() && !IsEmpty())
        {
            m_recv.Release(ctx);
        }

        return true;
    }

    bool Recv(Context* ctx)
    {
        if (TryRecv(ctx))
        {
            return true;
        }
        
        m_recv.Acquire(ctx);
        if (!RecvImpl())
        {
            assert(IsShutdown());
            m_recv.Release(ctx);
            return false;
        }

        if ((IsShutdown() && IsEmpty())|| !IsEmpty())
        {
            m_recv.Release(ctx);
        }

        if (m_send.IsHeld() && !IsFull())
        {
            m_send.Release(ctx);
        }

        return true;
    }

    void Shutdown(Context* ctx)
    {
        if (IsShutdown())
        {
            return;
        }
        m_shutdown = true;
        if (m_send.IsHeld())
        {
            m_send.Release(ctx);
        }

        if (m_recv.IsHeld())
        {
            m_recv.Release(ctx);
        }
    }

    bool IsShutdown() const
    {
        return m_shutdown;
    }

    bool IsEmpty() const
    {
        return m_size == 0;
    }

    bool IsFull() const
    {
        return m_size == m_capacity;
    }

private:
    bool SendImpl()
    {
        if (IsFull())
        {
            return false;
        }
        m_size++;
        return true;
    }

    bool RecvImpl()
    {
        if (IsEmpty())
        {
            return false;
        }
        m_size--;
        return true;
    }

    int m_size;
    int m_capacity;
    bool m_shutdown;
    Coordinator m_send;
    Coordinator m_recv;
};

// SizedChannel is a channel and requires no virtualization on the hot path for channel use; i's
// just sugar for co-allocating the channel's storage.
//
template<typename T, size_t C>
struct SizedChannel : Channel<T>
{
    SizedChannel()
    : Channel<T>(&m_list[0], C)
    {
    }

  private:
    T m_list[C];
};

} // end namespace coop
