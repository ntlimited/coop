# Per-Cooperator Timer Queue

A `Sleep` does not need its own kernel timer. The kernel can wake the cooperator once per *moment in
time it must be woken*, not once per *sleep that wants waking* — and at fan-out those are very
different numbers.

## Why this exists

coop's original `Sleep` armed one `IORING_OP_TIMEOUT` per call. Each one becomes a distinct kernel
`hrtimer`: the kernel arms it, queues it on a `timerqueue` red-black tree, reprograms the per-CPU
LAPIC deadline, takes a timer interrupt when it expires, and runs the expiry through
`__hrtimer_run_queues`. A workload that keeps N sleeps in flight therefore pushes N timers through
that machinery. Profiling an IO-suspending fan-out workload (a context per pipeline, each pipeline a
tight loop of *tiny compute → Sleep*, many pipelines, OS-scheduled across a few cores) found this was
coop's single largest *non-compute* cost: roughly 4–5% of cycles in `lapic_next_deadline`,
`timerqueue_add`, `__hrtimer_init`, `__hrtimer_run_queues` and the timer interrupt path. A peer
stackful work-stealing runtime that keeps a userspace deadline structure and arms one kernel timer
total spends only ~2% there. coop was paying per sleep what a userspace timer pays per *wakeup*.

The fix moves the deadline bookkeeping into userspace. A cooperator can own a deadline-ordered
structure of its in-flight sleeps. The kernel then holds at most **one** timer per cooperator — armed
for the nearest deadline — and a single expiry services *every* sleep that has come due, then re-arms
once for the new nearest. N concurrent sleeps cost O(1) kernel timers and one wakeup per distinct
service, not N of each.

This is deliberately *not* a replacement for `timer_slack_01.md`'s quantization. Slack collapses
*distinct deadlines* so fewer wakeups are needed; this collapses *distinct kernel timers* so each
deadline costs nothing in the kernel until it is actually the nearest. They compose: slack still
rounds deadlines onto a shared grid before they enter the queue, and the queue still arms one kernel
timer for whatever the nearest grid point turns out to be.

## Selectable, and off by default

The userspace queue is the newer, less-proven-at-scale path, so it does not displace the existing
one — it is selected per cooperator by `CooperatorConfiguration::timerMode`, and the default is the
proven kernel-per-timer behavior:

- **`TimerMode::KernelPerTimer`** (default): each sleep arms its own `IORING_OP_TIMEOUT`, exactly as
  before. Nothing about the default path changes; the queue is never touched, the scheduler's
  service/arm hooks see an empty queue and return on a single branch.
- **`TimerMode::UserspaceQueue`** (opt-in): the per-cooperator deadline queue described here.

It lands dormant so it can be proven by the benchmarks below and have its default flipped later,
matching the project's norm of landing unproven prototypes disabled. No adaptive auto-switch is
provided — the failure modes of guessing wrong mid-flight are harder to reason about than an explicit
choice — but the indicators that should drive the choice are clear, and they are about *how often a
timer is actually reached*:

- **High concurrent-timer count with a low timeout-hit frequency** — most timers are cancelled before
  they fire — strongly favors `UserspaceQueue`. A never-fired timer costs only a cheap intrusive
  insert and an O(log n) cancel in userspace, and *zero* kernel timer traffic, where kernel-per-timer
  pays the full arm-and-cancel hrtimer cost for a timer that never mattered.
- **Few timers, or timers whose deadlines are usually reached**, favor `KernelPerTimer`: there is
  little to coalesce, and the kernel path is simpler and battle-tested.

## The structure

The deadline structure is an **intrusive pairing heap** keyed by absolute `CLOCK_MONOTONIC`
microseconds (`coop/time/timer_queue.h`). The node (`time::TimerNode`) is embedded directly in the
`Sleeper`, so registering a sleep allocates nothing — the heap is pointer surgery over memory the
sleeping context already owns on its own stack.

A pairing heap was chosen over the red-black tree the issue suggested for one reason: correctness per
line. It gives O(1) insert, O(1) find-min (the root *is* the minimum), and amortized O(log n)
delete-min and arbitrary-delete, which is exactly the operation profile here — frequent inserts
(every `Sleep`), a find-min on every arm and every service, delete-min as deadlines expire, and
arbitrary delete on cancellation (kill mid-sleep, or a Grid stealer whose doorbell beats its park
timer). It carries none of the rebalancing-and-recolouring surface area of a red-black delete, so the
structure that has to be *unconditionally* correct on the cleanup path is small enough to audit by
reading it. "Far-future deadline" is a non-event for a heap — a deadline a year out is just a key
that never becomes the minimum until everything nearer has drained — so the cascading a hashed timing
wheel needs for far-future entries does not arise.

Find-min is the root pointer; no separate cached-leftmost is needed.

## Servicing and arming

The scheduler loop (`Cooperator::Launch`) drives the kernel timer at exactly two points, and a
sleeping context is always `BLOCKED`, which is what guarantees the loop reaches them:

- **Service** (`ServiceExpiredTimers`): pop every node whose deadline is `<= now` and `Release` its
  coordinator (`schedule=false`, so the woken context joins the yielded list rather than being
  switched to mid-loop). Called at the batch boundary after running contexts, and again in the idle
  branch after `Poll`. The batch-boundary call is what services sleeps in a *saturated* loop that
  never goes idle: the loop's own iteration rate is the clock, and no kernel timer is needed at all
  while there is other work to run.

- **Arm** (`ArmNearestTimer`): called only in the idle branch, immediately before the loop blocks in
  `WaitAndPoll`. It maintains the invariant *if the queue is non-empty, an armed kernel timer exists
  whose deadline is no later than the nearest queued deadline*. If nothing is armed it arms a fresh
  `IORING_OP_TIMEOUT` for the nearest deadline. If a timer is already armed but a *nearer* deadline
  has since appeared, it reschedules that one timer in place with `IORING_TIMEOUT_UPDATE` — one SQE,
  no cancel, no second timer object. An armed timer that turns out to be *earlier* than needed is
  left alone: it fires early, services nothing, and re-arms for the true nearest on the next idle
  pass. Early is harmless; late is the only thing the invariant forbids.

Why arming can be this simple: a cooperator mutates its own queue only while running one of its
contexts, and that only happens *between* `WaitAndPoll` calls. While the cooperator is blocked in the
kernel waiting on the timer, nothing on its thread can insert a nearer deadline, so there is no
"nearer deadline arrived while we were asleep" race to chase. A cross-thread `Submit` wakes the loop
via its eventfd CQE first; the submitted work runs, inserts its deadline, and the loop re-arms before
it next blocks.

The single timer is identified to io_uring by a small tagged userdata cookie (bit 2 marks a
deadline-timer CQE; bit 0 within that distinguishes the expiry from an update acknowledgement; the
owning cooperator is the thread's current one, so no pointer is encoded). It is deliberately *not*
counted in `Uring::PendingOps()`: a lingering far-future timer must not keep the shutdown loop alive,
and it need not, because sleeping contexts in `m_blocked` already hold the loop open exactly as long
as there are sleeps to service. A timer still in flight at shutdown is reaped by the ring teardown.

These two hooks are present unconditionally but inert under `KernelPerTimer`: with nothing ever
inserted, `ServiceExpiredTimers` and `ArmNearestTimer` both return on their leading empty-queue
check, so the default path keeps its exact previous behavior and pays only a single predictable
branch.

## Sleep and the Grid park

`time::Sleeper` (`coop/time/sleep.h`) carries both backings and picks between them on the owning
cooperator's `TimerMode`, transparently to the caller. Under `UserspaceQueue` it acquires its
`Coordinator`, registers an embedded `TimerNode`, and its destructor removes the node if still linked;
under `KernelPerTimer` it arms a per-sleep `IORING_OP_TIMEOUT` through an `io::Handle`, whose
destructor cancels a pending timeout. `Wait()` blocks kill-aware via `CoordinateWithKill` either way,
because both back the same coordinator. In the queue path the kill path needs no io_uring traffic at
all — the kill signal wakes the context, `CoordinateWithKill` returns `Killed`, and the destructor
unlinks the node in pure userspace. Either way cleanup is unconditional and allocation-free, including
on a killed context (an unsubmitted `Handle` and an unlinked node are both no-op teardowns).

The Grid idle stealer's recheck park (`work::Grid::StealerLoop`) is in scope and now parks through a
`Sleeper` too: it arms the sleeper, then `CoordinateWithKill`s on its doorbell *and* the sleeper's
coordinator together, so a local shed still wakes it immediately and the park timer still bounds
cross-cooperator steal latency. Because it goes through `Sleeper`, the park follows whichever
`TimerMode` the cooperator runs — a private `IORING_OP_TIMEOUT` per park by default, or a node on the
shared queue when the wheel is enabled.

## Extending to IO timeouts (the documented next step)

This v1 is scoped to pure timers, but the abstraction is deliberately not painted into a sleeps-only
corner: the queue keys on an absolute deadline and Releases a coordinator, which is all an IO timeout
needs too. The highest-value follow-up is to back IO-operation timeouts (`Recv`/`Send`/`Accept`/…)
with the same queue, for a reason that makes them an even *bigger* win than sleeps: an IO timeout is
usually **cancelled, not reached** — the operation completes before its deadline — and a userspace
queue pays only a cheap insert and an O(log n) cancel for a timer that never fires, versus the full
kernel arm-and-cancel hrtimer cost the linked-timeout path pays today on *every* IO op. That is
exactly the "high count, low hit-frequency" regime the indicators above call out.

It is **not** implemented here, and IO-operation timeouts remain **out of scope** for v1: today they
remain exact, per-operation linked timeouts (`Recv`/`Send`/`Accept`/`Connect`/`Resolve` and the
linked-timeout path are untouched), because an IO deadline guards correctness and must not be
coalesced onto a shared grid or serviced lazily before the path is proven. The note here is to record
the direction and keep the deadline abstraction reusable, not to widen the v1 surface.

## Covenants

Negative-first — what this design forbids:

- **No cross-thread state.** The timer queue, the armed-timer bookkeeping (`m_timerArmed`,
  `m_timerDeadlineUs`), and every read/write of them live on the owning cooperator's thread only.
  There are no atomics and no shared state in the timer path; the structure must never be made
  reachable from another thread, and a sleep registered on one cooperator must never be serviced by
  another.

- **A sleep never fires early.** The queue is one-sided: a deadline may be serviced no earlier than
  the requested interval has elapsed. An armed kernel timer may fire *earlier* than the nearest
  deadline (a harmless spurious wake that re-arms), but `ServiceExpiredTimers` releases a coordinator
  only when `deadline <= now`, so no context resumes from `Sleep` before its time. This preserves
  `timer_slack_01.md`'s one-sided covenant unchanged: slack rounds deadlines *later*, the queue fires
  them no *earlier*, and IO-guarding deadlines carry no slack and stay exact.

- **No new per-sleep allocation.** Registering a sleep must not allocate, in either mode. Under
  `UserspaceQueue` the `TimerNode` is embedded in the `Sleeper` and the heap is intrusive; a design
  that reintroduced a per-sleep heap node or closure would defeat the purpose. Under `KernelPerTimer`
  the embedded `io::Handle` is unchanged.

- **The default path does not change.** The userspace queue lands disabled
  (`TimerMode::KernelPerTimer`). A cooperator that does not opt in must behave exactly as before —
  one `IORING_OP_TIMEOUT` per sleep — and the scheduler's service/arm hooks must stay inert (empty
  queue) on that path. The opt-in must never silently become the default; flipping it is a separate,
  evidence-backed change.

- **No change to the `Sleep` API or semantics.** `Sleep(ctx, interval[, slack])` still blocks for the
  interval and returns `Ok` / `Killed` / `Error`, still kill-aware, still RAII-clean on a killed
  context, in both modes. The win is entirely under the existing surface; a caller cannot tell the
  backing changed except by measuring the kernel-timer count.

- **The Sleep path does not touch the Coordinator or continuation hot path.** The red guards —
  `BM_AcquireRelease` (~1.05ns) and the continuation-fire benches (~8ns) — must be unchanged. This
  work adds a `Release` *caller* (the timer service) and a heap; it does not alter
  `Acquire`/`Release`/`TryAcquire` or continuation dispatch.

- **IO timeouts are not coalesced.** The single-kernel-timer coalescing applies only to `Sleep` and
  the Grid park. An IO-operation timeout must remain an exact, per-operation linked timeout. The
  timer queue must never be repurposed to back one.
