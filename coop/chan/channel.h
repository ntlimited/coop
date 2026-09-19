#pragma once

#include <cassert>
#include <utility>

#include "coop/coordinator.h"
#include "coop/coordinate_with.h"
#include "coop/self.h"

// Go-style directional channels: Channel<T> is chan T; RecvChannel / SendChannel are
// <-chan T / chan<- T — the same buffer, restricted operations. They are pointer-sized
// views, not virtual bases, so Channel::Recv/Send are direct member access (no vbptr).
// Pipe, Select, and friends take the view by value (a Channel& converts). Capture the
// view, not a reference to a temporary view.
//
// Select across channels: CoordinateWith on m_recv (or m_send), then RecvAcquired /
// SendAcquired on the winner:
//
//   auto r = CoordinateWithKill(ctx, &ch1.m_recv, &ch2.m_recv);
//   if (r.Killed())            { ... }
//   else if (r == &ch1.m_recv) { ch1.RecvAcquired(v1); }
//   else                       { ch2.RecvAcquired(v2); }
//
// Invariant (outside shutdown): empty ↔ m_recv held; full ↔ m_send held.
// m_recv starts held. m_send starts held only for capacity 0 (rendezvous).
//

namespace coop
{
namespace chan
{

// Kill-oblivious Acquire, or CoordinateWithKill. Returns false if kill won (the
// coordinator was not acquired). Recv/Send stay kill-oblivious so destructor
// cleanup can still wait; RecvKill/SendKill are the Group-cancellation path.
//
inline bool WaitCoordinator(Coordinator* c, Context* ctx, bool killAware)
{
    if (!killAware)
    {
        c->Acquire(ctx);
        return true;
    }
    return !CoordinateWithKill(ctx, c).Killed();
}

inline bool ShutdownEnds(bool& shutdown, Coordinator& recv, Coordinator& send)
{
    if (shutdown)
    {
        return false;
    }
    shutdown = true;
    Context* ctx = Self();
    if (recv.IsHeld())
    {
        recv.Release(ctx);
    }
    if (send.IsHeld())
    {
        send.Release(ctx);
    }
    return true;
}

// Shared coordinators + shutdown for Channel<void>. Not a virtual base.
//
struct BaseChannel
{
    BaseChannel(Context* ctx)
    : m_shutdown(false)
    , m_recv(ctx)
    {
    }

    bool IsShutdown() const { return m_shutdown; }
    bool Shutdown();

    Coordinator m_recv;
    Coordinator m_send;

  protected:
    bool m_shutdown;
};

template<typename T>
struct Channel
{
    Channel(const Channel&) = delete;
    Channel(Channel&&) = delete;

    Channel(Context* ctx, T* buffer, size_t capacity)
    : m_recv(ctx)
    , m_send()
    , m_buffer(buffer)
    , m_head(0)
    , m_tail(0)
    , m_size(0)
    , m_capacity(capacity)
    , m_shutdown(false)
    {
        if (!m_capacity)
        {
            m_send.Acquire(ctx);
        }
    }

    bool IsShutdown() const { return m_shutdown; }
    bool Shutdown() { return ShutdownEnds(m_shutdown, m_recv, m_send); }

    bool IsEmpty() const { return m_size == 0; }
    bool IsFull() const { return m_size == m_capacity; }

    bool TryRecv(T& value /* out */)
    {
        if (IsEmpty())
        {
            return false;
        }

        RecvImpl(value);
        Context* ctx = Self();

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

    bool Recv(T& value /* out */) { return RecvWait(value, false); }
    bool RecvKill(T& value /* out */) { return RecvWait(value, true); }

    // Pull up to maxCount available items non-blockingly. Returns the number of items
    // drained. Releases m_send with schedule=false when unblocking a waiting sender.
    //
    size_t Drain(T* data, size_t maxCount)
    {
        if (IsEmpty()) return 0;

        Context* ctx = Self();
        size_t n = 0;

        while (n < maxCount && !IsEmpty())
        {
            bool wasFull = IsFull();
            RecvImpl(data[n++]);

            if (wasFull && m_send.IsHeld())
                m_send.Release(ctx, false);
        }

        if (IsEmpty() && !IsShutdown())
            m_recv.Acquire(ctx);

        return n;
    }

    bool RecvAcquired(T& value)
    {
        Context* ctx = Self();

        if (!RecvImpl(value))
        {
            assert(IsShutdown());
            m_recv.Release(ctx);
            return false;
        }

        if ((IsShutdown() && IsEmpty()) || !IsEmpty())
            m_recv.Release(ctx);

        if (m_send.IsHeld() && !IsFull())
            m_send.Release(ctx);

        return true;
    }

    bool TrySend(T value)
    {
        if (IsShutdown() || IsFull())
        {
            return false;
        }

        [[maybe_unused]] bool sent = SendImpl(std::move(value));
        assert(sent);
        Context* ctx = Self();

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

    bool Send(T value) { return SendWait(std::move(value), false); }
    bool SendKill(T value) { return SendWait(std::move(value), true); }

    // Push all items in [data, data+count), blocking when full. Intermediate m_recv
    // releases use schedule=false so the consumer is deferred until the batch is
    // complete or the buffer is full.
    //
    bool SendAll(const T* data, size_t count)
    {
        if (IsShutdown()) return false;

        Context* ctx = Self();

        bool acquiredSend = false;
        for (size_t i = 0; i < count; i++)
        {
            while (IsFull())
            {
                m_send.Acquire(ctx);
                acquiredSend = true;
                if (IsShutdown())
                {
                    m_send.Release(ctx);
                    return false;
                }
            }

            bool wasEmpty = IsEmpty();
            [[maybe_unused]] bool ok = SendImpl(data[i]);
            assert(ok);

            if (IsFull())
                m_send.TryAcquire(ctx);
            else if (acquiredSend && i == count - 1)
                m_send.Release(ctx, false);

            if (wasEmpty && m_recv.IsHeld())
                m_recv.Release(ctx, i == count - 1);
        }

        return true;
    }

    bool SendAcquired(T value)
    {
        Context* ctx = Self();

        if (IsShutdown())
        {
            m_send.Release(ctx);
            return false;
        }

        [[maybe_unused]] bool ok = SendImpl(std::move(value));
        assert(ok);

        if (!IsFull())
            m_send.Release(ctx);

        if (m_recv.IsHeld() && !IsEmpty())
            m_recv.Release(ctx);

        return true;
    }

    Coordinator m_recv;
    Coordinator m_send;

  private:
    bool RecvWait(T& value, bool killAware)
    {
        if (TryRecv(value))
        {
            return true;
        }

        Context* ctx = Self();

        if (IsShutdown())
        {
            if (m_recv.IsHeld())
            {
                m_recv.Release(ctx);
            }
            return false;
        }

        if (!WaitCoordinator(&m_recv, ctx, killAware))
        {
            return false;
        }

        while (!RecvImpl(value))
        {
            if (IsShutdown())
            {
                m_recv.Release(ctx);
                return false;
            }

            if (!WaitCoordinator(&m_recv, ctx, killAware))
            {
                return false;
            }
        }

        if ((IsShutdown() && IsEmpty()) || !IsEmpty())
        {
            m_recv.Release(ctx);
        }

        if (m_send.IsHeld() && !IsFull())
        {
            m_send.Release(ctx);
        }

        return true;
    }

    bool RecvImpl(T& value)
    {
        if (IsEmpty())
        {
            return false;
        }

        value = std::move(m_buffer[m_head++]);
        if (m_head == m_capacity)
        {
            m_head = 0;
        }
        m_size--;
        return true;
    }

    bool SendWait(T value, bool killAware)
    {
        if (!IsShutdown() && !IsFull())
        {
            [[maybe_unused]] bool sent = SendImpl(std::move(value));
            assert(sent);
            Context* ctx = Self();

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

        Context* ctx = Self();
        if (!WaitCoordinator(&m_send, ctx, killAware))
        {
            return false;
        }
        if (IsShutdown())
        {
            m_send.Release(ctx);
            return false;
        }

        [[maybe_unused]] bool sent = SendImpl(std::move(value));
        assert(sent);

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

    bool SendImpl(T value)
    {
        if (IsFull())
        {
            return false;
        }
        m_buffer[m_tail++] = std::move(value);
        if (m_tail == m_capacity)
        {
            m_tail = 0;
        }
        m_size++;
        return true;
    }

    T* m_buffer;
    size_t m_head;
    size_t m_tail;
    size_t m_size;
    size_t m_capacity;
    bool m_shutdown;
};

// <-chan T. Pointer-sized; Channel& converts. Capture by value across Spawn.
//
template<typename T>
struct RecvChannel
{
    Channel<T>* ch{};

    RecvChannel() = default;
    RecvChannel(Channel<T>& c) : ch(&c) {}

    Coordinator* RecvCoord() const { return &ch->m_recv; }

    bool IsShutdown() const { return ch->IsShutdown(); }
    bool Shutdown() { return ch->Shutdown(); }
    bool IsEmpty() const { return ch->IsEmpty(); }
    bool IsFull() const { return ch->IsFull(); }

    bool TryRecv(T& value) { return ch->TryRecv(value); }
    bool Recv(T& value) { return ch->Recv(value); }
    bool RecvKill(T& value) { return ch->RecvKill(value); }
    size_t Drain(T* data, size_t maxCount) { return ch->Drain(data, maxCount); }
    bool RecvAcquired(T& value) { return ch->RecvAcquired(value); }
};

// chan<- T.
//
template<typename T>
struct SendChannel
{
    Channel<T>* ch{};

    SendChannel() = default;
    SendChannel(Channel<T>& c) : ch(&c) {}

    Coordinator* SendCoord() const { return &ch->m_send; }

    bool IsShutdown() const { return ch->IsShutdown(); }
    bool Shutdown() { return ch->Shutdown(); }
    bool IsEmpty() const { return ch->IsEmpty(); }
    bool IsFull() const { return ch->IsFull(); }

    bool TrySend(T value) { return ch->TrySend(std::move(value)); }
    bool Send(T value) { return ch->Send(std::move(value)); }
    bool SendKill(T value) { return ch->SendKill(std::move(value)); }
    bool SendAll(const T* data, size_t count) { return ch->SendAll(data, count); }
    bool SendAcquired(T value) { return ch->SendAcquired(std::move(value)); }
};

// Channel<void> — counting channel (no ring). Same capacity contract as Channel<T>.
//
template<>
struct Channel<void> : BaseChannel
{
    Channel(const Channel&) = delete;
    Channel(Channel&&) = delete;

    Channel(Context* ctx, size_t capacity)
    : BaseChannel(ctx)
    , m_count(0)
    , m_capacity(capacity)
    {
        if (!m_capacity)
        {
            m_send.Acquire(ctx);
        }
    }

    bool IsEmpty() const { return m_count == 0; }
    bool IsFull()  const { return m_count == m_capacity; }

    bool TrySend()
    {
        if (IsShutdown() || IsFull()) return false;

        m_count++;
        Context* ctx = Self();

        if (IsFull())
            m_send.Acquire(ctx);

        if (m_recv.IsHeld())
            m_recv.Release(ctx);

        return true;
    }

    bool Send() { return SendWait(false); }
    bool SendKill() { return SendWait(true); }

    bool TryRecv()
    {
        if (IsEmpty()) return false;

        m_count--;
        Context* ctx = Self();

        if (IsEmpty())
            m_recv.Acquire(ctx);

        if (m_send.IsHeld())
            m_send.Release(ctx);

        return true;
    }

    bool Recv() { return RecvWait(false); }
    bool RecvKill() { return RecvWait(true); }

    bool RecvAcquired()
    {
        Context* ctx = Self();

        if (!RecvImpl())
        {
            assert(IsShutdown());
            m_recv.Release(ctx);
            return false;
        }

        if ((IsShutdown() && IsEmpty()) || !IsEmpty())
            m_recv.Release(ctx);

        if (m_send.IsHeld() && !IsFull())
            m_send.Release(ctx);

        return true;
    }

    bool SendAcquired()
    {
        Context* ctx = Self();

        if (IsShutdown())
        {
            m_send.Release(ctx);
            return false;
        }

        m_count++;

        if (!IsFull())
            m_send.Release(ctx);

        if (m_recv.IsHeld() && !IsEmpty())
            m_recv.Release(ctx);

        return true;
    }

  private:
    bool RecvWait(bool killAware)
    {
        if (TryRecv()) return true;

        Context* ctx = Self();

        if (IsShutdown())
        {
            if (m_recv.IsHeld())
                m_recv.Release(ctx);
            return false;
        }

        if (!WaitCoordinator(&m_recv, ctx, killAware))
        {
            return false;
        }

        while (!RecvImpl())
        {
            if (IsShutdown())
            {
                m_recv.Release(ctx);
                return false;
            }
            if (!WaitCoordinator(&m_recv, ctx, killAware))
            {
                return false;
            }
        }

        if ((IsShutdown() && IsEmpty()) || !IsEmpty())
            m_recv.Release(ctx);

        if (m_send.IsHeld() && !IsFull())
            m_send.Release(ctx);

        return true;
    }

    bool SendWait(bool killAware)
    {
        if (TrySend()) return true;

        Context* ctx = Self();
        if (!WaitCoordinator(&m_send, ctx, killAware))
        {
            return false;
        }
        if (IsShutdown())
        {
            m_send.Release(ctx);
            return false;
        }

        m_count++;

        if (!IsFull())
            m_send.Release(ctx);

        if (m_recv.IsHeld() && !IsEmpty())
            m_recv.Release(ctx);

        return true;
    }

    bool RecvImpl()
    {
        if (IsEmpty()) return false;
        m_count--;
        return true;
    }

    size_t m_count;
    size_t m_capacity;
};

template<typename T, size_t N>
struct FixedChannel : Channel<T>
{
    explicit FixedChannel(Context* ctx)
    : Channel<T>(ctx, m_buf, N)
    {}
    T m_buf[N];
};

template<size_t N>
struct FixedChannel<void, N> : Channel<void>
{
    explicit FixedChannel(Context* ctx) : Channel<void>(ctx, N) {}
};

} // end namespace chan
} // end namespace coop
