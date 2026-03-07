#pragma once

#include <utility>

#include "coordinator.h"
#include "self.h"

// This is a pretty close facsimile of golang's channel implementation and is meant to be a second
// order coordination primitive on top of the Coordinator.
//
// Select across channels: use CoordinateWith on m_recv (or m_send for send-select), then call
// RecvAcquired (or SendAcquired) on the winning channel to complete the operation:
//
//   auto r = CoordinateWithKill(ctx, &ch1.m_recv, &ch2.m_recv);
//   if (r.Killed())            { ... }
//   else if (r == &ch1.m_recv) { ch1.RecvAcquired(v1); }
//   else                       { ch2.RecvAcquired(v2); }
//
// The channel invariant (m_recv not held ↔ channel non-empty) means CoordinateWith's TryAcquire
// fast path fires immediately when a channel already has data.
//
// TODO: the main value here would be that in theory, this supporting `Shutdown` would make the
// coordinator not have to natively support it instead (e.g. we want the "always immediately select
// the cancelled channel" semantics from golang). However the nuance above regarding our version
// (multi-coordinator patterns) means that we're kind of stuck between things.
//

namespace coop
{
namespace chan
{

// NOTE: there is almost certainly a good amount of refactoring (I would hope?) around all of the
// subtly different logic in the send/recv implementations. Or maybe not and that's why it's
// subtly different.
//
// The basic contract here is that we have two coordinators used to allow blocking on the channel's
// two sides: e.g. waiting for someone to send when we're recv-ing, and waiting for someone to recv
// when we're send-ing. This is complicated by the 'shutdown' semantic but not that terribly. In
// short, we need to make sure the following invariants are true (outside of the shutdown case):
//
//  - If the channel is empty, the recv coordinator is held
//  - If the channel is full, the send coordinator is held
//
// This means that we must start out with m_recv held initially in all cases, and that we must
// start out with m_send held in the 0-capacity channel case.
//
struct BaseChannel
{
    BaseChannel(Context* ctx)
    : m_shutdown(false)
    , m_recv(ctx)
    {
    }

  protected:
    // Required for virtual inheritance — the most-derived class initializes the virtual base
    // directly, but intermediate classes still need a default constructor to exist syntactically.
    //
    BaseChannel()
    : m_shutdown(false)
    {
    }

  public:

    virtual ~BaseChannel()
    {
    }

    bool IsShutdown() const
    {
        return m_shutdown;
    }

    bool Shutdown()
    {
        if (m_shutdown)
        {
            return false;
        }

        m_shutdown = true;
        Context* ctx = Self();

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

    TypedBaseChannel(Context* ctx, T* buffer, size_t capacity)
    : BaseChannel(ctx)
    , m_buffer(buffer)
    , m_head(0)
    , m_tail(0)
    , m_size(0)
    , m_capacity(capacity)
    {
        if (!m_capacity)
        {
            m_send.Acquire(ctx);
        }
    }

  protected:
    // See BaseChannel default constructor comment
    //
    TypedBaseChannel()
    : m_buffer(nullptr)
    , m_head(0)
    , m_tail(0)
    , m_size(0)
    , m_capacity(0)
    {
    }

  public:

    virtual ~TypedBaseChannel()
    {
    }

    bool IsEmpty() const
    {
        return m_size == 0;
    }

    bool IsFull() const
    {
        return m_size == m_capacity;
    }

  protected:
    T* m_buffer;
    size_t m_head;
    size_t m_tail;
    size_t m_size;
    size_t m_capacity;
};

// If there's a way to get rid of the `Base::` stuttering everywhere I didn't find or remember one.
//
template<typename T>
struct RecvChannel : virtual TypedBaseChannel<T>
{
    using Base = TypedBaseChannel<T>;

    bool TryRecv(T& value /* out */)
    {
        if (Base::IsEmpty())
        {
            return false;
        }

        RecvImpl(value);
        Context* ctx = Self();

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
    bool Recv(T& value /* out */)
    {
        if (TryRecv(value))
        {
            return true;
        }

        Context* ctx = Self();

        // TryRecv returned false: channel is empty (cooperative scheduling guarantees this
        // is still true here). If already shut down, nothing more will ever arrive. Release
        // m_recv if we hold it (TryRecv may have acquired it when draining the last item)
        // to propagate the shutdown to any other waiting receivers.
        //
        if (Base::IsShutdown())
        {
            if (Base::m_recv.IsHeld())
            {
                Base::m_recv.Release(ctx);
            }
            return false;
        }

        Base::m_recv.Acquire(ctx);

        // Spurious-wakeup loop: Drain and TryRecv pre-acquire m_recv while still running
        // (not blocking), so a sender may release m_recv to an empty wait list, leaving
        // m_recv un-held. When we then Acquire it lands immediately even though the channel
        // is still empty. In that case m_recv is now held by us; re-Acquire to actually
        // block until the sender adds an item or the channel is shut down.
        //
        while (!RecvImpl(value))
        {
            if (Base::IsShutdown())
            {
                Base::m_recv.Release(ctx);
                return false;
            }

            // m_recv is now held by us — self-block until a sender releases it.
            //
            Base::m_recv.Acquire(ctx);
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

    // Pull up to maxCount available items non-blockingly. Returns the number of items drained.
    // Returns 0 immediately if the channel is empty. Releases m_send with schedule=false when
    // unblocking a waiting sender, so the caller can process the batch before the sender refills.
    //
    size_t Drain(T* data, size_t maxCount)
    {
        if (Base::IsEmpty()) return 0;

        Context* ctx = Self();
        size_t n = 0;

        while (n < maxCount && !Base::IsEmpty())
        {
            bool wasFull = Base::IsFull();
            RecvImpl(data[n++]);

            if (wasFull && Base::m_send.IsHeld())
                Base::m_send.Release(ctx, false);
        }

        if (Base::IsEmpty() && !Base::IsShutdown())
            Base::m_recv.Acquire(ctx);

        return n;
    }

    // Complete a recv after m_recv has been acquired externally via CoordinateWith. The caller
    // is responsible for having acquired m_recv (the invariant that m_recv not held ↔ non-empty
    // guarantees RecvImpl will succeed unless the channel was shut down).
    //
    bool RecvAcquired(T& value)
    {
        Context* ctx = Self();

        if (!RecvImpl(value))
        {
            assert(Base::IsShutdown());
            Base::m_recv.Release(ctx);
            return false;
        }

        if ((Base::IsShutdown() && Base::IsEmpty()) || !Base::IsEmpty())
            Base::m_recv.Release(ctx);

        if (Base::m_send.IsHeld() && !Base::IsFull())
            Base::m_send.Release(ctx);

        return true;
    }

  protected:
    bool RecvImpl(T& value)
    {
        if (Base::IsEmpty())
        {
            return false;
        }

        value = std::move(Base::m_buffer[Base::m_head++]);
        if (Base::m_head == Base::m_capacity)
        {
            Base::m_head = 0;
        }
        Base::m_size--;
        return true;
    }
};

template<typename T>
struct SendChannel : virtual TypedBaseChannel<T>
{
    using Base = TypedBaseChannel<T>;

    bool TrySend(T value)
    {
        if (Base::IsShutdown() || Base::IsFull())
        {
            return false;
        }

        [[maybe_unused]] bool sent = SendImpl(std::move(value));
        assert(sent);
        Context* ctx = Self();

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
    bool Send(T value)
    {
        // Fast path: cannot call TrySend(value) here because for move-only T, TrySend takes
        // its argument by value (consuming it), and we need the value preserved if the channel
        // is full so the blocking path can send it. Inline the fast path instead.
        //
        if (!Base::IsShutdown() && !Base::IsFull())
        {
            [[maybe_unused]] bool sent = SendImpl(std::move(value));
            assert(sent);
            Context* ctx = Self();

            if (Base::IsFull())
            {
                Base::m_send.Acquire(ctx);
            }

            if (Base::m_recv.IsHeld())
            {
                Base::m_recv.Release(ctx);
            }
            return true;
        }

        // Blocking path: note that IsShutdown() can get flipped while we were coordinating
        //
        Context* ctx = Self();
        Base::m_send.Acquire(ctx);
        if (Base::IsShutdown())
        {
            Base::m_send.Release(ctx);
            return false;
        }

        [[maybe_unused]] bool sent = SendImpl(std::move(value));
        assert(sent);

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

        return true;
    }

    // Push all items in [data, data+count), blocking when the channel is full. Returns false if
    // shutdown before all items were sent. Unlike calling Send() in a loop, intermediate m_recv
    // releases use schedule=false so the consumer is deferred until the batch is complete or the
    // buffer is full, reducing context switches when filling from an empty channel.
    //
    bool SendAll(const T* data, size_t count)
    {
        if (Base::IsShutdown()) return false;

        Context* ctx = Self();

        for (size_t i = 0; i < count; i++)
        {
            if (Base::IsFull())
            {
                Base::m_send.Acquire(ctx);
                if (Base::IsShutdown())
                {
                    Base::m_send.Release(ctx);
                    return false;
                }
            }

            [[maybe_unused]] bool ok = SendImpl(data[i]);
            assert(ok);

            if (Base::IsFull())
                Base::m_send.Acquire(ctx);

            if (Base::m_recv.IsHeld())
                Base::m_recv.Release(ctx, i == count - 1);
        }

        return true;
    }

    // Complete a send after m_send has been acquired externally via CoordinateWith. The caller
    // is responsible for having acquired m_send (the invariant that m_send not held ↔ non-full
    // guarantees SendImpl will succeed unless the channel was shut down).
    //
    bool SendAcquired(T value)
    {
        Context* ctx = Self();

        if (Base::IsShutdown())
        {
            Base::m_send.Release(ctx);
            return false;
        }

        [[maybe_unused]] bool ok = SendImpl(std::move(value));
        assert(ok);

        if (!Base::IsFull())
            Base::m_send.Release(ctx);

        if (Base::m_recv.IsHeld() && !Base::IsEmpty())
            Base::m_recv.Release(ctx);

        return true;
    }

  protected:
    bool SendImpl(T value)
    {
        if (Base::IsFull())
        {
            return false;
        }
        Base::m_buffer[Base::m_tail++] = std::move(value);
        if (Base::m_tail == Base::m_capacity)
        {
            Base::m_tail = 0;
        }
        Base::m_size++;
        return true;
    }
};

template<typename T>
struct Channel : RecvChannel<T>, SendChannel<T>
{
    Channel(Context* ctx, T* buffer, size_t capacity)
    : TypedBaseChannel<T>(ctx, buffer, capacity)
    {
    }

  protected:
    // For subclasses that initialize the virtual base (TypedBaseChannel) directly.
    // Without this, the most-derived class has no default Channel<T> constructor to call
    // after it handles the virtual base initialization itself.
    //
    Channel() = default;
};

// Channel<void> — counting channel (no ring buffer, no value). Semantics are identical
// to Channel<T> except that send/recv transfer no data: senders increment a count, receivers
// decrement it. Useful as a first-class signaling primitive that participates in Select.
//
// Capacity follows the same contract as Channel<T>: capacity 0 is a rendezvous channel
// (sender blocks until a receiver is ready), capacity N buffers up to N pending signals.
//
// Use On(ch, callback) where callback takes no arguments.
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

    bool Send()
    {
        if (TrySend()) return true;

        Context* ctx = Self();
        m_send.Acquire(ctx);
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

    bool Recv()
    {
        if (TryRecv()) return true;

        Context* ctx = Self();

        if (IsShutdown())
        {
            if (m_recv.IsHeld())
                m_recv.Release(ctx);
            return false;
        }

        m_recv.Acquire(ctx);

        while (!RecvImpl())
        {
            if (IsShutdown())
            {
                m_recv.Release(ctx);
                return false;
            }
            m_recv.Acquire(ctx);
        }

        if ((IsShutdown() && IsEmpty()) || !IsEmpty())
            m_recv.Release(ctx);

        if (m_send.IsHeld() && !IsFull())
            m_send.Release(ctx);

        return true;
    }

    // Complete a recv after m_recv has been acquired externally via CoordinateWith.
    //
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

    // Complete a send after m_send has been acquired externally via CoordinateWith.
    //
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
    bool RecvImpl()
    {
        if (IsEmpty()) return false;
        m_count--;
        return true;
    }

    size_t m_count;
    size_t m_capacity;
};

// Fixed-capacity channel that owns its buffer. Eliminates the parallel buffer declaration
// that Channel<T> requires at the call site.
//
//   coop::FixedChannel<int, 4> ch(ctx);
//
// Virtual base construction note: FixedChannel is the most-derived class, so it must
// initialize TypedBaseChannel<T> (the virtual base) directly. Channel<T>'s own initializer
// for the virtual base is skipped in that context.
//
template<typename T, size_t N>
struct FixedChannel : Channel<T>
{
    explicit FixedChannel(Context* ctx)
    : TypedBaseChannel<T>(ctx, m_buf, N)
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
