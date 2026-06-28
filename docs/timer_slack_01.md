# Timer-Slack Quantization

A sleep does not usually care about its deadline to the microsecond. It cares that it wakes *around*
then. That looseness is worth money, because coop pays per distinct deadline.

## Why this exists

coop has no timer wheel. Every sleep arms its own `IORING_OP_TIMEOUT`, and the kernel backs each
one with a distinct hrtimer. So a population of concurrent sleeps at slightly-different deadlines
arms a distinct kernel timer per deadline, and the kernel wakes the cooperator thread once per
distinct expiry — one `io_uring_enter` return, one scheduler pass, per wakeup. Most of those wakeups
exist only because the deadlines failed to coincide by a few microseconds; the work each one does is
trivial, the wakeup overhead is not.

Slack collapses the coincidence. A non-zero slack rounds a sleep's absolute deadline **up** to the
next multiple of the slack interval. Nearby deadlines then land on a shared kernel expiry, so the
kernel fires one timer and delivers a batch of completions where it would otherwise have fired a
scatter of near-simultaneous ones. This is userspace `PR_SET_TIMERSLACK` (whose default is 50us) and
the hashed timing wheel's natural bucket granularity (Varghese & Lauck), expressed in coop's
microsecond `Interval`.

The cost is bounded and one-sided: at most one slack interval of over-sleep, never an early wake.

## Where it applies

Coalescing requires *multiple concurrent timers competing on one ring*. The beneficiary is therefore
a fan-out of concurrent sleeps on one cooperator — many in-flight `Sleep` deadlines that the slack
grid pulls together. It is opt-in per call: `Sleep(ctx, interval, slack)`, default `slack = 0`
(exact), so existing callers are unchanged.

The idle stealer's recheck park (`work::Grid`) is *not* a beneficiary, despite being a tolerant
deadline: each cooperator parks a single recheck timer on its own ring at a time, and a lone timer
has nothing to coalesce with. Quantizing it would only phase-align independent rings' wakeups, for no
batching gain. The park is left exact. The lever for the park's tail is a larger recheck interval
backed by cross-thread steal-wake, not slack.

## Measured

Probe: a fan-out of N sleeps with deadlines jittered across a window wide enough that the cooperator
genuinely idles between expiries (measured with a standalone probe). Headline metric is
`RUSAGE_THREAD` voluntary context switches on the cooperator thread — the count of kernel timer
wakeups the lever is meant to collapse. One machine, release, warm, pinned; N=500 over a 20ms window
(~40us mean inter-deadline spacing):

| slack | kernel wakeups | over-sleep mean | over-sleep max |
|-------|----------------|-----------------|----------------|
| exact | ~400           | ~7 us           | ~30 us         |
| 10 us | ~420           | ~10 us          | ~50 us         |
| 50 us | ~285           | ~33 us          | ~65 us         |
| 100 us| ~185           | ~64 us          | ~125 us        |
| 500 us| ~42            | ~265 us         | ~525 us        |

The knee sits where the slack grid approaches the inter-deadline spacing. A 50–100us grid — the same
neighbourhood as Linux's default task slack — sheds 30–55% of wakeups for tens of microseconds of
over-sleep; a 500us grid sheds ~90%. A grid *below* the inter-deadline spacing (10us here) buys
nothing: there is no coincidence to find, so it only adds jitter. Over-sleep tracks the model: mean
≈ grid/2, max ≈ grid.

In a saturated regime — deadlines arriving faster than the loop drains them — the thread never idles,
so there are no per-timer wakeups to collapse and slack neither helps nor hurts the wakeup count
(it still bounds over-sleep). The win is specifically the sparse, idle-between-timers regime.

## Covenants

Negative-first — what this design forbids:

- **No slack on a correctness deadline.** A deadline that guards a protocol or IO operation
  (`Recv`/`Send`/`Connect`/`Resolve` timeouts, the linked-timeout IO path) must remain exact. A
  protocol deadline allowed to drift is a bug, not an optimization. This is enforced structurally,
  not by convention: the IO timeout path carries no slack parameter at all. Slack lives only on the
  `Sleep` path, where the deadline is explicitly a hint.
- **Slack is never silent.** It is opt-in per call with a default of zero. No call site acquires
  over-sleep it did not ask for; introducing slack to a path is a visible code change at that path.
- **Slack is one-sided.** It may only round a deadline *later*, never earlier. A sleep must never
  return before its requested interval has elapsed.
- **Slack does not reach across cooperators.** It is a per-ring deadline alignment, not a global
  schedule. It must not be repurposed into a cross-cooperator synchronization mechanism.
