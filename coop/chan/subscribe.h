#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

#include "coop/chan/channel.h"
#include "coop/coordinate_with.h"
#include "coop/coordinator.h"
#include "coop/detail/coordinator_extension.h"
#include "coop/self.h"
#include "coop/thunk.h"

// Subscribe / Drain -- continuation-dispatched fan-in over N channels into per-channel handlers,
// with NO parked consumer context and NO per-delivery heap allocation.
//
//   auto sub = coop::chan::Subscribe(ctx,
//       coop::chan::Drain(chA, [&](Msg&& m)              { ... }),
//       coop::chan::Drain(chB, [&](Job&& j, Control& c)  { if (done) c.Stop(); }));
//   sub.Wait();   // join: returns once every channel has shut down
//
// ---------------------------------------------------------------------------
// Why this exists
// ---------------------------------------------------------------------------
//
// Ordinary channel Recv parks the consumer CONTEXT: a producer's Send releases m_recv, the
// scheduler switches into the parked stack, the body runs, the stack re-blocks -- two context
// switches per item. A Subscription replaces the parked context with a CONTINUATION per channel (a
// coop::Thunk). A producer's Send releases m_recv, which queues the continuation; the cooperator
// loop's drain Runs it as a plain FUNCTION CALL -- no context switch, no parked stack. Each handler
// runs inside its channel's continuation.
//
// ---------------------------------------------------------------------------
// Object-hosted re-arm -- the no-heap evolution
// ---------------------------------------------------------------------------
//
// Each Drain case becomes an Arm that lives INSIDE the Subscription's storage (a member, not a heap
// node). The Arm holds a Coordinated hook and registers itself on its channel's m_recv. When it
// fires, it drains the channel and runs the handler, then RE-REGISTERS THE SAME OBJECT on m_recv --
// zero allocation per fire. Re-arming in place is safe because by the time Run() executes, the
// scheduler's drain has already popped the Arm's Coordinated node off both the coordinator wait list
// and the pending-continuation queue: the node is fully disconnected, so AddAsBlocked re-links it
// cleanly with nothing to corrupt. Object-hosting is also what makes an Arm cancellable for free: its
// node can be unlinked (RemoveAsBlocked) from wherever it sits, which a pooled/detached continuation
// could not offer.
//
// ---------------------------------------------------------------------------
// The handler is a Thunk -- it must not suspend
// ---------------------------------------------------------------------------
//
// Each handler runs as a Thunk: run-to-completion, MUST NOT suspend. No Yield(), no Block(), no
// blocking IO, no blocking channel Recv/Send, no CoordinateWith inside the handler -- any of those
// would stall the host context, not the (stackless) thunk. coop::detail::ThunkScope arms an assert
// in debug; a suspend trips AssertNotInThunk at the misuse site. The verb is spelled Drain, not
// On/Select, to telegraph at the call site that these bodies are drains, not blocking selects.
//
// ---------------------------------------------------------------------------
// Drain-all-per-fire (a correctness requirement, not an optimization)
// ---------------------------------------------------------------------------
//
// A channel's m_recv is EDGE-triggered: Send releases it only on the empty -> non-empty transition.
// A Send into an already-non-empty channel does NOT re-pulse m_recv, so between two fires a producer
// may deposit many items. Each Arm therefore drains its channel to EMPTY on every fire via
// Channel::Drain -- which re-Acquires m_recv when it empties (restoring the empty <-> m_recv-held
// invariant for the next Send to re-pulse) and wakes a blocked sender with schedule=false, the only
// correct wake from the context-free continuation drain. (Channel::TryRecv wakes with schedule=true,
// which has no context to switch from here and asserts.)
//
// ---------------------------------------------------------------------------
// Lifetime and shutdown
// ---------------------------------------------------------------------------
//
// A Subscription is a named local that OBJECT-HOSTS its arms; it is neither copyable nor movable
// (the arms capture their own member addresses on coordinator wait lists). When a channel shuts
// down, its Arm drains the tail, observes IsShutdown, and retires (no re-arm). When the LAST arm
// retires, a completion coordinator is released; Wait()/WaitKill() block on it. ~Subscription()
// Cancel()s every still-live arm, so the channels' m_recv wait lists are empty by the time they die.
//

namespace coop
{
namespace chan
{

// Control is the thin token handed to handlers that ask for it (h(T&&, Control&)). Both verbs are
// DEFERRED-TO-RETURN: they only set a flag; the Arm reads it after the handler returns and acts
// then. This is what makes shutdown-from-inside-a-continuation safe -- the Arm is mid-fire on the
// stack when the handler runs, so it cannot tear itself (or its siblings) down underfoot; it waits
// until control is back in its own hands.
//
//   ctl.Stop()       retire THIS arm  (do not re-arm; subsequent sends to it are not delivered)
//   ctl.CancelAll()  retire the WHOLE subscription
//
struct Control
{
    void Stop()       { m_stop = true; }
    void CancelAll()  { m_cancelAll = true; }

    bool m_stop      = false;
    bool m_cancelAll = false;
};

namespace detail
{
    template<typename>
    inline constexpr bool kAlwaysFalse = false;

    // Pass ONLY what the handler asks for ("every stack op costs"). Prefer the control-taking
    // signature when the handler accepts it, else the value-only signature. The chain is left open
    // for documented follow-ups -- h(T&&, ptrdiff_t) demand, h(Channel<T>&) level -- which slot in
    // as additional requires-dispatched branches without disturbing these two.
    //
    template<typename H, typename T>
    inline void Dispatch(H& h, T&& value, Control& ctl)
    {
        if constexpr (std::is_invocable_v<H&, T&&, Control&>)
        {
            h(std::forward<T>(value), ctl);
        }
        else if constexpr (std::is_invocable_v<H&, T&&>)
        {
            h(std::forward<T>(value));
        }
        else
        {
            static_assert(kAlwaysFalse<H>,
                "Drain handler must be callable as h(T&&) or h(T&&, Control&)");
        }
    }
}

// Non-template state shared by every Arm of one Subscription: the completion coordinator, the live
// count, the cancel flag, and the flat array of arm pointers (so Cancel/CancelAll can retire arms of
// heterogeneous types uniformly). Separated out so an Arm<T,H> can call back without knowing the
// Subscription's full (variadic) type.
//
struct SubscriptionCore;

// ArmBase carries everything an Arm does that is NOT typed by (T, H): the Coordinated hook, the
// back-pointer to the core, the channel's m_recv, and the retire bookkeeping. Retire() is the single
// point that unlinks the node and accounts the retirement exactly once.
//
struct ArmBase : Continuation
{
    ArmBase(SubscriptionCore* core, Coordinator* recv)
    : m_coordinated(static_cast<Continuation*>(this))
    , m_core(core)
    , m_recv(recv)
    {
    }

    ArmBase(ArmBase const&) = delete;
    ArmBase(ArmBase&&)      = delete;

    // Register (or re-register) this arm on its channel's m_recv wait list. Called once at
    // construction and again in place at the tail of every non-terminal Run().
    //
    void Register()
    {
        CoordinatorExtension().AddAsBlocked(m_recv, &m_coordinated);
        m_listed = true;
    }

    // Retire this arm exactly once: unlink its node from whatever list it currently sits on, then
    // tell the core. While "listed", the node sits on either (a) m_recv's wait list (armed) or
    // (b) the cooperator's pending-continuation queue (released, awaiting Run) -- in both it is
    // genuinely linked with valid neighbour pointers, so EmbeddedListHookups::Pop unlinks it
    // correctly via its OWN pointers regardless of which list holds it.
    //
    // m_listed is tracked explicitly rather than read from Coordinated::Disconnected(): the
    // scheduler's drain pops our node off the pending queue (EmbeddedList::Pop) before calling Run,
    // and in RELEASE that Pop does NOT null the node's pointers (it does only under NDEBUG). So a
    // running/retired node has stale non-null neighbours -- Disconnected() would wrongly report it
    // still linked, and Pop-ing it would corrupt the list it was already removed from. m_listed is
    // cleared at Run entry (the drain just unlisted us) and after a Pop here, so it is the only
    // release-correct witness of list membership. Single-threaded, so membership cannot shift
    // underfoot. Inlined below the core.
    //
    void Retire();

    Coordinated       m_coordinated;
    SubscriptionCore* m_core;
    Coordinator*      m_recv;
    bool              m_listed  = false;
    bool              m_retired = false;
};

struct SubscriptionCore
{
    explicit SubscriptionCore(Context* ctx)
    : m_completion(ctx)   // starts HELD; released once the last arm retires
    {
    }

    SubscriptionCore(SubscriptionCore const&) = delete;
    SubscriptionCore(SubscriptionCore&&)      = delete;

    // Account one arm retiring. When the last one goes, release the completion coordinator so a
    // parked Wait()/WaitKill() wakes. The release is context-free (it can run from the drain), hence
    // schedule=false; Release does not dereference the Context*, so nullptr is fine.
    //
    void OnArmRetired()
    {
        if (--m_liveArms == 0)
        {
            m_completion.Release(nullptr, /*schedule=*/false);
        }
    }

    // Retire every arm now. Used by Subscription::Cancel() (from a context) and by a handler's
    // Control::CancelAll() (from inside a fire). Set the cancel flag first so any sibling that is
    // already queued in the pending-continuation drain bails when its Run() reaches it -- though in
    // practice Retire() also unlinks such a sibling out of the pending queue, so it never Runs.
    //
    void RetireAll()
    {
        m_cancelRequested = true;
        for (int i = 0; i < m_count; i++)
        {
            m_arms[i]->Retire();
        }
    }

    // Join: block until the completion coordinator is released (every arm retired). Acquiring it
    // re-holds it, so release it again to keep it latched-open for repeat/parallel waiters and for
    // the already-complete fast path.
    //
    void WaitComplete(Context* ctx)
    {
        CoordinateWith(ctx, &m_completion);
        m_completion.Release(ctx);
    }

    // Kill-aware join: returns false if the context is killed before completion. On the kill branch
    // the completion coordinator was not acquired, so nothing to release.
    //
    bool WaitCompleteKill(Context* ctx)
    {
        auto result = CoordinateWithKill(ctx, &m_completion);
        if (result.Killed())
        {
            return false;
        }
        m_completion.Release(ctx);
        return true;
    }

    Coordinator m_completion;
    ArmBase**   m_arms = nullptr;
    int         m_count = 0;
    int         m_liveArms = 0;
    bool        m_cancelRequested = false;
};

inline void ArmBase::Retire()
{
    if (m_retired)
    {
        return;
    }
    m_retired = true;
    if (m_listed)
    {
        m_coordinated.Pop();   // unlink from m_recv wait list OR pending queue, whichever holds it
        m_listed = false;
    }
    m_core->OnArmRetired();
}

// One Drain case, fully typed. Holds the typed channel and handler; Run() drains + dispatches +
// re-arms (or retires). Constructed in place inside the Subscription's storage.
//
template<typename T, typename H>
struct Arm final : ArmBase
{
    static constexpr size_t kBatch = 16;

    // The arm does NOT register in its constructor: it has no channel traffic to react to until the
    // SubscriptionCore is fully wired (m_arms/m_count/m_liveArms set), because a courtesy fire's
    // handler may CancelAll, which walks every arm. Subscription's ctor body calls ArmInitial() once
    // per arm after that wiring is done.
    //
    Arm(SubscriptionCore* core, RecvChannel<T>* channel, H handler)
    : ArmBase(core, &channel->m_recv)
    , m_channel(channel)
    , m_handler(std::move(handler))
    {
    }

    // Initial arming, run once from Subscribe's context after the core is wired. If the channel
    // already holds buffered data at subscribe time, adopt it now (a "courtesy fire") rather than
    // strand it until the next Send -- m_recv is edge-triggered, and a Send into an
    // already-non-empty channel would not pulse it. The courtesy fire runs the SAME drain path as a
    // normal fire (FireOnce -> Channel::Drain), which re-Acquires m_recv when it empties, so the arm
    // ends correctly registered on a held m_recv (empty <-> m_recv-held invariant restored) and
    // future Sends re-pulse it. We are on a context here (not the continuation drain), so Drain's
    // m_recv.Acquire-on-empty is an ordinary context acquire; the ThunkScope keeps the handler's
    // no-suspend contract enforced exactly as the drain would.
    //
    void ArmInitial()
    {
        if (m_retired || m_core->m_cancelRequested)
        {
            return;
        }
        if (m_channel->IsEmpty())
        {
            // A channel already shut down before we subscribed will never fire us (its Shutdown
            // already pulsed m_recv to an empty wait list), so registering would strand the arm and
            // hang Wait(). Retire it straight away instead.
            //
            if (m_channel->IsShutdown())
            {
                Retire();
                return;
            }
            Register();
            return;
        }

        ::coop::detail::ThunkScope inThunk;
        if (FireOnce())
        {
            Register();
        }
    }

    void Run() final
    {
        // The drain popped our node off the pending queue to call us, so we are no longer listed.
        // (In release Pop leaves stale neighbour pointers; m_listed, not Disconnected(), is the
        // correct witness -- see ArmBase::Retire.)
        //
        m_listed = false;

        // A sibling's CancelAll (or an external Cancel) may have fired while this arm sat in the
        // pending queue. Either path already unlinked + retired us; bail without touching freed or
        // re-purposed state.
        //
        if (m_retired || m_core->m_cancelRequested)
        {
            return;
        }

        if (FireOnce())
        {
            Register();                    // re-arm IN PLACE -- same object, no allocation
        }
    }

    // Drain the channel to EMPTY, dispatch each item, then decide the arm's fate. Returns true if
    // the arm should (re-)register on m_recv, false if it has retired. Shared by Run (per fire) and
    // ArmInitial (courtesy fire). Channel::Drain re-Acquires m_recv on emptying and wakes a blocked
    // sender with schedule=false. Stop/CancelAll break the drain so no further items are delivered
    // once the handler asks to retire.
    //
    bool FireOnce()
    {
        T       batch[kBatch];
        Control ctl;
        bool    done = false;
        size_t  n;
        while (!done && (n = m_channel->Drain(batch, kBatch)) > 0)
        {
            for (size_t i = 0; i < n; i++)
            {
                detail::Dispatch(m_handler, std::move(batch[i]), ctl);
                if (ctl.m_stop || ctl.m_cancelAll)
                {
                    done = true;
                    break;
                }
            }
        }

        // Deferred-to-return: now that the handler has returned and control is back in the arm's
        // hands, act on what it asked for.
        //
        if (ctl.m_cancelAll)
        {
            m_core->RetireAll();           // retires this arm and every sibling
            return false;
        }
        if (ctl.m_stop || m_channel->IsShutdown())
        {
            Retire();                      // terminal
            return false;
        }
        return true;
    }

    RecvChannel<T>* m_channel;
    H               m_handler;
};

// A Drain case before it is hosted: the channel and handler, plus the concrete Arm type to build.
// Subscribe moves these into the Subscription, which constructs the Arms at their final addresses.
//
template<typename T, typename H>
struct DrainSpec
{
    using ArmType = Arm<T, H>;

    RecvChannel<T>* m_channel;
    H               m_handler;
};

// Drain(ch, handler) -- declare an arm. T is deduced from the channel (derived-to-base deduction
// handles Channel<T>/FixedChannel), H from the handler.
//
template<typename T, typename H>
[[nodiscard]] DrainSpec<T, H> Drain(RecvChannel<T>& channel, H handler)
{
    return DrainSpec<T, H>{&channel, std::move(handler)};
}

namespace detail
{
    // Recursive inline storage for the heterogeneous arms -- one member per arm, constructed in
    // place (the arms are non-movable; a std::tuple cannot host them, and there is no heap node).
    //
    template<typename... Specs>
    struct ArmStorage;

    template<>
    struct ArmStorage<>
    {
        explicit ArmStorage(SubscriptionCore*) {}
        void Collect(ArmBase**, int&) {}
        void ArmAll() {}
    };

    template<typename Spec, typename... Rest>
    struct ArmStorage<Spec, Rest...>
    {
        ArmStorage(SubscriptionCore* core, Spec spec, Rest... rest)
        : m_head(core, spec.m_channel, std::move(spec.m_handler))
        , m_tail(core, std::move(rest)...)
        {
        }

        void Collect(ArmBase** out, int& i)
        {
            out[i++] = &m_head;
            m_tail.Collect(out, i);
        }

        // Initial arming over the typed storage -- a compile-time fold, no virtual dispatch. Runs
        // after the core is wired so a courtesy fire's CancelAll can safely walk every arm.
        //
        void ArmAll()
        {
            m_head.ArmInitial();
            m_tail.ArmAll();
        }

        typename Spec::ArmType m_head;
        ArmStorage<Rest...>    m_tail;
    };
}

// Subscription -- the pure-virtual interface to a live fan-in. The subscription-LEVEL operations
// (Cancel / Wait / WaitKill) are coarse and per-subscription, so a single vtable dispatch on each is
// free; the hot per-item path (the arms) stays non-virtual and object-hosted in the concrete type.
// This is coop's three-level idiom: a pure-virtual interface at the API boundary, a concrete
// (template) implementation that owns the data. Callers that do not care about the arm types hold a
// Subscription& or Subscription*, e.g. to collect heterogeneous subscriptions and cancel them all:
//
//   std::vector<coop::chan::Subscription*> subs;
//   subs.push_back(&subA); subs.push_back(&subB);
//   for (auto* s : subs) s->Cancel();
//
struct Subscription
{
    Subscription() = default;
    Subscription(Subscription const&) = delete;
    Subscription(Subscription&&)      = delete;
    virtual ~Subscription() = default;

    // Retire every arm now. Idempotent.
    //
    virtual void Cancel() = 0;

    // Join: block the caller until every arm has retired (all channels shut down). NOT kill-aware.
    //
    virtual void Wait() = 0;

    // Kill-aware join: returns false if the caller is killed before completion.
    //
    virtual bool WaitKill() = 0;
};

namespace detail
{
    // The concrete fan-in: object-hosts its arms (no heap), implements the Subscription interface.
    // Non-copyable and non-movable -- the arms capture their own member addresses on coordinator
    // wait lists. Build it with Subscribe.
    //
    template<typename... Specs>
    struct SubscriptionImpl final : Subscription
    {
        static constexpr int N = static_cast<int>(sizeof...(Specs));

        SubscriptionImpl(Context* ctx, Specs... specs)
        : m_core(ctx)
        , m_storage(&m_core, std::move(specs)...)
        {
            int i = 0;
            m_storage.Collect(m_armPtrs, i);
            m_core.m_arms     = m_armPtrs;
            m_core.m_count    = N;
            m_core.m_liveArms = N;

            // No arms -> already complete; release the completion coordinator so Wait() returns at
            // once.
            //
            if (N == 0)
            {
                m_core.m_completion.Release(nullptr, /*schedule=*/false);
            }

            // Arm every case now that the core is wired (a courtesy fire's CancelAll may walk all
            // arms). This is also where already-buffered data is adopted -- see Arm::ArmInitial.
            //
            m_storage.ArmAll();
        }

        ~SubscriptionImpl() final
        {
            Cancel();
        }

        void Cancel() final     { m_core.RetireAll(); }
        void Wait() final       { m_core.WaitComplete(Self()); }
        bool WaitKill() final   { return m_core.WaitCompleteKill(Self()); }

      private:
        SubscriptionCore     m_core;
        ArmStorage<Specs...> m_storage;
        ArmBase*             m_armPtrs[N > 0 ? N : 1];
    };
}

// Subscribe(ctx, Drain(...), Drain(...), ...) -- arm a continuation-dispatched fan-in. Returns the
// concrete subscription constructed in place (guaranteed copy elision), so the arms register at
// their final addresses. Bind it to a named local that outlives the traffic; pass it by
// Subscription& / Subscription* where the arm types do not matter:
//
//   auto sub = coop::chan::Subscribe(ctx, coop::chan::Drain(ch, handler));
//
template<typename... Specs>
[[nodiscard]] detail::SubscriptionImpl<Specs...> Subscribe(Context* ctx, Specs... specs)
{
    return detail::SubscriptionImpl<Specs...>(ctx, std::move(specs)...);
}

} // end namespace chan
} // end namespace coop
