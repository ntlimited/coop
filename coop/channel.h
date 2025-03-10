#pragma once

#include "coordinator.h"

// This is a pretty close facscimile of golang's channel implementation and is meant to be a second
// order coordination primitive on top of the Coordinator. However while the coordinator currently
// has some nice building block pieces including a golang select over channel implementation (e.g.,
// (CoordinateWith), this doesn't have a `select`.
//
// This may well be a technical dead end and anything higher level than coordinators to this degree
// misses the point of coordinators and their intended built-in composibility; e.g. if the
// coordinatewith mechanic isn't sufficient then what's the point? Or the channel system would
// simply use that itself under the hood and that's okay.
//
// Either way it was fun making the diamond pattern work.
//

namespace coop
{

// NOTE: there is almost certainly a good amount of refactoring (I would hope?) around all of the
// subtly different logic in the send/recv implementations. Or maybe not and that's why it's
// subtly different.
//
struct BaseChannel
{
    BaseChannel()
    : m_shutdown(false)
    {
    }

    virtual ~BaseChannel()
    {
    }

    bool IsShutdown() const
    {
        return m_shutdown;
    }

    bool Shutdown(Context* ctx)
    {
        if (m_shutdown)
        {
            return false;
        }

        m_shutdown = true;

        if (m_recv.IsHeld())
        {
            m_recv.Release(ctx);
        }
        if (m_send.IsHeld())
        {
            m_send.Release(ctx);
        }

        return true;
    }

    Coordinator m_recv;
    Coordinator m_send;

  private:
    bool m_shutdown;
};

template<typename T>
struct TypedBaseChannel : public BaseChannel
{
  public:
    TypedBaseChannel(TypedBaseChannel const& other) = delete;
    TypedBaseChannel(TypedBaseChannel&&) = delete;

    TypedBaseChannel(T* buffer, size_t capacity)
    : m_buffer(buffer)
    , m_head(0)
    , m_tail(0)
    , m_capacity(capacity)
    {
    }

    virtual ~TypedBaseChannel()
    {
    }
  
    bool IsEmpty() const
    {
        return m_head == m_tail;
    }

    bool IsFull() const
    {
        auto next = (m_tail + 1) % m_capacity;
        return next ==  m_head;
    }

  private:
    T* m_buffer;
    size_t m_head;
    size_t m_tail;
    size_t m_capacity;
};

// If there's a way to get rid of the `Base::` stuttering everywhere I didn't find or remember one.
//
template<typename T>
struct RecvChannel : virtual TypedBaseChannel<T>
{
    using Base = TypedBaseChannel<T>;

    bool TryRecv(Context* ctx, T& value /* out */)
    {
        if (Base::IsEmpty())
        {
            return false;
        }

        RecvImpl(ctx, value);

        if (Base::IsEmpty())
        {
            Base::m_recv.Acquire(ctx);
        }

        if (Base::m_send.IsHeld())
        {
            Base::m_send.Release(ctx);
        }

        return true;
    }
    
    // Mirror image of the above but we handle shutdown differently
    //
    bool Recv(Context* ctx, T& value /* out */)
    {
        if (TryRecv(ctx, value))
        {
            return true;
        }

        Base::m_recv.Acquire(ctx);

        // This is possible in the shutdown case
        //
        if (!RecvImpl(value))
        {
            assert(Base::IsShutdown());
            Base::m_recv.Release(ctx);
            return false;
        }

        // If we are shutdown, no more things can come through so signal anyone else who is waiting
        // on the coordinator; alternatively, if there is more to recv, then let go of it for the
        // next coordinatee.
        //
        if ((Base::IsShutdown() && Base::IsEmpty()) || !Base::IsEmpty())
        {
            Base::m_recv.Release(ctx);
        }

        if (Base::m_send.IsHeld() && !Base::IsFull())
        {
            Base::m_send.Release(ctx);
        }

        return true;
    }

  protected:
    bool RecvImpl(T& value)
    {
        if (Base::IsEmpty())
        {
            return false;
        }

        value = Base::m_buffer[Base::m_head++];
        if (Base::m_head == Base::m_capacity)
        {
            Base::m_head = 0;
        }
        return true;
    }
};

template<typename T>
struct SendChannel : virtual TypedBaseChannel<T>
{
    using Base = TypedBaseChannel<T>;

    bool TrySend(Context* ctx, T value)
    {
        if (Base::IsShutdown() || Base::IsFull())
        {
            return false;
        }

        SendImpl(value);
        
        if (Base::IsFull())
        {
            Base::m_send.Acquire(ctx);
        }

        // Note that this is not the same as the non-try case
        //
        if (Base::m_recv.IsHeld())
        {
            Base::m_recv.Release(ctx);
        }
        return true;
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
        Base::m_send.Acquire(ctx);
        if (Base::IsShutdown())
        {
            Base::m_send.Release(ctx);
            return false;
        }

        Base::SendImpl(value);
        
        // If there is space, let go of the coordinator so that the preconditions still stand. This
        // in theory wouldn't happen today but if we change the unblock mechanics to not just
        // chain immediately it's very possible for multiple Recvs before this wakes up holding the
        // coordinator.
        //
        if (!Base::IsFull())
        {
            Base::m_send.Release(ctx);
        }

        if (Base::m_recv.IsHeld() && !Base::IsEmpty())
        {
            Base::m_recv.Release(ctx);
        }
    }

  protected:
    bool SendImpl(Context* ctx, T value)
    {
        if (Base::IsFull())
        {
            return false;
        }
        Base::m_buffer[Base::m_tail++] = value;
        if (Base::m_tail == Base::m_capacity)
        {
            Base::m_tail = 0;
        }
        return true;
    }
};

template<typename T>
struct Channel : RecvChannel<T>, SendChannel<T>
{
    Channel(T* buffer, size_t capacity)
    : TypedBaseChannel<T>(buffer, capacity)
    {
    }
};

template<>
struct Channel<void> : BaseChannel
{
    Channel(int capacity, int used = 0)
    : m_size(used)
    , m_capacity(capacity)
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
