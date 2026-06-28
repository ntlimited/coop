# Cross-Thread Work Substrate 01

Status: **governing design** — the implementation contract for the work-sharing substrate.
The premise has been measured (see *Measured result* below); what remains is to formalize it
in-tree against the covenants and calibration here.

## Where we are

Single-cooperator continuations are done and proven end-to-end (see `continuations_01.md`):

- Structured (frame-hosted, awaited) and detached (pooled, self-owning) continuations,
  ~8ns dispatch either way.
- Fire from real io_uring CQEs with zero added plumbing.
- 227x less memory per in-flight pipeline than a parked context (72B vs 16KB), and the
  per-pipeline advance cost stays flat where context-per-pipeline degrades under cache
  pressure (5.4x at 64 pipelines widening to 12.7x at 4096).

The hard constraint that made all of that cheap: **a continuation is registered, fired,
and cancelled on one cooperator and never migrates.** No atomics, cancellation race-free
by construction.

## The gap

That buys us in-cooperator decomposition. It does **not** move work across cores. Today
the only cross-thread path is `Cooperator::Submit` — a push to a *named* cooperator over a
mutex-protected intrusive FIFO with an eventfd wake per submission. That is the right tool
for "run this on *that* cooperator" (affinity, targeted wake). It is the wrong tool for
"run this *somewhere with capacity*" — it requires the producer to choose a target, and a
bad choice cannot be rebalanced.

Our own balancing experiments, and a study of a production stackful-fiber work-stealing
runtime built for a closely related space, landed on a clear bet: for irregular compute, **work-queue
(morsel) balancing beats fiber/context migration**, and a **single global queue
negative-scales** — it must be sharded. That stackful model migrates whole fibers; we intend to shed
cheap run-to-completion *morsels* and never migrate the execution context at all.

## The bet

> Continuations stay in-cooperator. To cross cores, a continuation *body* sheds a separate
> **task** into a sharded, work-stealing pool. The continuation node never crosses; only a
> task (a copy of the work) does.

This keeps the in-cooperator fast path untouched and confines all cross-thread atomics to
the substrate. The substrate is the morsel-sharing layer the balancing findings asked for,
and the thing we measure against the stackful model.

## Measured result (premise proven)

A benchmark-level prototype of this design went from being outperformed to outperforming a
production stackful-fiber work-stealing runtime, on the exact workload such work-stealing targets. Workload: M=6 cooperators, 6000 IO pipelines, each alternating
irregular `BusyFor` compute with an io_uring timed sleep; *clustered* imbalance (all heavy
pipelines land on one static shard).

| approach | clustered makespan (median) | efficiency |
|----------|-----------------------------|------------|
| coop context-per-pipeline, static shard | ~148 ms | 24% |
| coop continuation-per-pipeline, static shard | ~153 ms | 23% |
| **stackful-fiber work-stealing runtime** | **~74 ms** | 47% |
| **coop pull-pool prototype (this design)** | **~53 ms** | 66% |

The prototype: all pipelines in a shared pool; each cooperator runs a worker that pulls a
ready stage, runs its compute, submits the timer, and a detached continuation re-sheds the
pipeline to the pool when the timer fires. Static sharding can't move clustered load
(~150 ms); the pull-pool spreads it across whoever is free and wins 1.4x over the stackful model. It is
also robust — ~52 ms on the *mild* (already-balanced) workload too, where the stackful runtime runs ~65 ms.

The prototype's known weaknesses are exactly the formalization work: it spin-yields when idle
(burns cores, causes variance), uses a single ring rather than sharded deques, and runs
outside the scheduler loop. The compute floor is ~35 ms, so a proper wake protocol has
headroom to push ~53 → ~40 ms.

## Approach

1. **Per-cooperator shard, work-stealing deque.** Each cooperator owns one Chase-Lev-style
   deque. The owner pushes/pops its own end LIFO (recently-shed work is cache-hot); idle
   thieves steal the far end FIFO (oldest task, likely the largest remaining subtree).
   This is the structure Go, Rayon, and similar runtimes converged on; no reason to be cleverer first.

2. **Pull, not push.** `Shed(task)` enqueues to the *local* shard (or the caller's nearest
   shard if shed from a non-cooperator thread). An idle cooperator — at the same point in
   the loop where it finds no runnable contexts and no IO — tries its own shard, then
   steals from others, then sleeps. Producers never choose a consumer.

3. **NUMA-aware stealing (phase 2).** Steal order: local shard → same-NUMA-node shards →
   cross-node. The CPU sampler already tags samples with the active `Cooperator*`; shard
   topology can reuse the cooperator registry.

4. **rseq for the owner's local end (phase 3).** Cooperators are core-pinned, so the owner's
   push/pop is almost always on its home CPU. A restartable sequence lets that path skip the
   atomic in the common case and abort only on the rare preemption/migration. Thieves still
   use the atomic far-end protocol. This is an optimization layered on a correct atomic
   baseline — not the baseline itself.

5. **Wake protocol.** Reuse the existing eventfd: a `Shed` that grows a previously-empty
   shard, or a successful steal target, wakes one sleeping cooperator. Idle cooperators
   spin-then-steal-then-sleep; this folds into the existing "no runnable work → WaitAndPoll"
   branch rather than adding a second sleep path.

6. **What a pulled task is.** A run-to-completion morsel by default (runs on the puller's
   green thread, like a detached continuation — no context spawned). A task that may block
   asks to be run on its own context (spawn). Default morsel = cheapest; blocking = opt-in.

## Decisions (resolved by the measured result)

- **`Shed` sits beside `Submit`, not replacing it.** `Submit` = targeted ("that cooperator",
  affinity); `Shed` = balanced ("anywhere with capacity"). Different intents; collapsing them
  loses the affinity case.
- **Shard granularity: per-cooperator deque**, with NUMA-aware *steal order* (not per-NUMA
  shards). Matches core pinning; one owner per deque keeps the owner's end uncontended.
- **A re-shed is a local push.** The benchmark re-sheds to a shared pool; in-tree the
  completion continuation pushes to *its own* cooperator's deque, and an idle cooperator
  steals it. Same balancing, no global queue.
- **Task representation: a cross-thread-owned pooled closure node.** The single-cooperator
  continuation pool cannot back it (a stolen task is freed on a different core than it was
  shed). The task pool is a per-cooperator slab with cross-thread free — the one genuinely new
  concurrency primitive this design introduces.
- **Fairness vs locality: start with classic Chase-Lev (LIFO-local, FIFO-steal) and measure.**
  No anti-starvation knob until the calibration shows one is needed.

## Implementation slices

- **Slice 1 — bounded Chase-Lev deque** (`work_deque.h`), standalone, weak-memory-correct
  (Le et al. ordering for the aarch64 target), with single-owner and concurrent-steal tests.
  No scheduler coupling. *Supporting structure — does not close a covenant on its own.*
- **Slice 2 — `Shed` API + explicit pull**, driven by workers (as the prototype was), proving
  the deque + task pool in-tree without touching the loop. Reproduces ~53 ms.
- **Slice 3 — opt-in participation + park policy** (reshaped by the Submit measurement below).
  *Not* core-loop surgery and *not* a Submit rebuild. A participating cooperator runs a daemon
  **stealer** context (spawned on opt-in) that pulls local / steals, runs the morsel, and when
  idle parks via `CoordinateWith(stealCoord, recheckTimeout)`. An in-cooperator `Shed`/re-shed
  pushes to the local deque and releases `stealCoord` (immediate local pickup); cross-cooperator
  rebalancing happens when an idle stealer's `recheckTimeout` fires and it steals an overloaded
  peer. This sidesteps the ~2.3µs cross-thread park-wake entirely (the stealer self-wakes via its
  own timer CQE or a local coord release — both in-cooperator), so there is **no cross-thread wake
  protocol and no change to `cooperator.cpp`'s loop**. The stealer is just a context; the
  Cooperator core is untouched, which is how opt-in stays free. `recheckTimeout` is the tunable
  park-policy parameter (start ~50µs, tune against the scoreboard). *This closes the covenant.*
- **Slice 4 — optimize:** adaptive recheck backoff (**landed** — see *Adaptive recheck backoff*
  below), then chunked steals (batch claims cut coordination cost — measured in the balancing
  experiments), then a shared injector for `Shed`-from-non-cooperator and smeared work (layered on
  Submit), then NUMA-aware steal order, then the rseq owner-end fast path, then the per-cooperator
  task slab.

## Submit measurement (why Slice 3 reshaped)

Measured `Cooperator::Submit` to decide whether to rebuild it (scratchpad `submit_bench.cpp`):

| metric | value |
|--------|-------|
| round-trip to a **parked** cooperator (tail) | ~2.3 µs |
| round-trip to a **warm** (not-yet-parked) cooperator | ~0.6 µs |
| single-producer throughput | ~600 ns/submit (1.4–1.7 M/s) |
| contended (2/4/8 producers) | 439 / 431 / 506 ns/submit — scales; mutex is not the bottleneck |

Conclusion: Submit's dominant cost is the **OS thread-wake floor (~2.3 µs)**, which no queue
mechanism changes; its throughput is **spawn-dominated** (a context per task), not mutex-dominated.
So Submit's mechanism is not the weakness, and combined with the opt-in covenant the decision is to
**leave Submit unchanged**. The actionable lesson for the substrate: the ~2.3 µs cross-thread wake
is expensive relative to nanosecond steal ops, so the stealer must **avoid cross-thread wakes** —
hence the timed self-recheck park above instead of a wake protocol.

## Opt-in covenant (load-bearing)

The work-sharing subsystem is **optional and opt-in**. A cooperator that does not opt in must
pay nothing — no deque allocated, no steal check on any path it runs, no participation in the
idle-set or wake protocol, and byte-for-byte the current scheduler loop on its hot path. This is
a hard requirement, not an aspiration, and it dictates the layering:

- **Submit stays the base cross-thread primitive.** It is core and widely used (~42 sites), so it
  must not be rebuilt *on* the work-stealing layer — that would make the opt-in subsystem
  mandatory. Submit may be improved on its own merits if measurement shows its mutex-list /
  eventfd-drainer is weak, but that is a separate, independent change.
- **Work-stealing layers on/beside Submit, never under it.** Participating cooperators get a
  deque + idle-path pull/steal + `Shed`; cross-thread `Shed` is expressed *via* Submit (deposit to
  a participant), not by making Submit traverse the steal deques.
- **Opt-in is explicit** (a `CooperatorConfiguration` flag / joining a work-sharing group). The
  idle-path steal is gated on participation; a non-participant's idle path is the current
  `WaitAndPoll`, unchanged.

**Proof obligation (part of the calibration):** a non-participant cooperator must show *zero*
regression — `BM_AcquireRelease`, the scheduler yield benchmarks, and a no-op-loop benchmark must
be unchanged with the subsystem compiled in but unused. If opting out is not free, the design is
wrong.

## Non-goals / what this must not become

- **No continuation migration.** The continuation node is single-cooperator; only task
  copies cross. If a change needs a continuation to cross cores, the change is wrong.
- **No global single queue.** Sharded from day one — the global queue negative-scales
  (measured).
- **No fiber/context migration.** The bet is morsel-level balancing; we do not move stacks.
- **No cross-thread atomics on the in-cooperator continuation/coordinator hot path.** The
  substrate's atomics live entirely in the deque, not in `Coordinator`/`CompletionLatch`.
- **No producer-side target selection for balanced work.** If `Shed` requires the caller to
  name a consumer, it has become `Submit` and missed the point.

## Calibration (red/green — the slice is not done until these hold)

The `io_fanout` scoreboard (promoted into `benchmarks/` as the coop-only `BM_FanOut_Pool`
family) is the instrument.

- **Green (CLOSED):** driven through the in-tree `work::Grid` (Erg pipeline stages re-shedding via
  `Shed` from their timer continuation), clustered makespan is a stable **~44 ms (80% of the 35 ms
  floor)** vs the stackful model's ~74 ms -- **1.7x** -- and ~43.5 ms on the mild workload vs ~65 ms. The
  idle stealer's recheck interval bounds the worst-case rebalancing latency (default 10us keeps the
  clustered case stable; 30us showed an occasional outlier). A cross-thread steal-wake would let the
  recheck be larger and tighten the tail further -- a Slice 4 refinement, not needed to close.
- **Red guard (the hot path is untaxed):** `BM_AcquireRelease` stays at 1.04 ns and the
  continuation dispatch benchmarks are unchanged. The substrate must add zero cost to the
  in-cooperator path — if a deque field or a steal check lands on the `Coordinator` /
  `CompletionLatch` / scheduler hot path, the change is wrong.
- **Deque calibration:** the steal/pop race tests must include a known-bad case (a naive
  ordering that loses or duplicates an item under stress) proven to fail, alongside the
  correct version passing — red/green for the data structure itself, not just green at head.
- **Robustness:** the mild (already-balanced) workload must not regress meaningfully against
  static shared-nothing — confirming `Shed` is an opt-in relief valve, not a tax on the
  balanced common case.

## Adaptive recheck backoff (Slice 4)

A fixed recheck interval taxes idle cores. The interval has to be small to keep the
clustered-imbalance tail tight, but a fully quiescent core then arms a fresh timer on every tick and
wakes for nothing — measured at **~60k wakeups/s per idle core at 10µs** on a 4-cooperator grid
(an effective ~16µs period once io_uring overhead is counted). Four idle cores is a quarter-million
wakeups a second spent discovering there is nothing to steal.

The stealer's park interval is therefore adaptive (`Grid::StealerLoop`): per-stealer locals, no
atomics, no shared state. It starts at `recheckMin`, and after `idleGrowAfter` consecutive empty
re-checks doubles each empty re-check up to `recheckMax`; **any** successful pull (local work or a
steal) snaps it back to `recheckMin`. So a stealer that is finding work stays aggressive, and only a
persistently idle core coasts to the cap. `recheckMin` bounds rebalancing latency while load is
present; `recheckMax` bounds it after the core has gone fully idle.

Measured (4 cooperators; fixed 10µs vs the same floor adapting to a cap):

| policy | idle wakeups/s/core | clustered makespan | cold-pickup latency (after full idle) |
|--------|--------------------:|-------------------:|--------------------------------------:|
| fixed 10µs        | ~59k | 10.26 ms | 2.1 µs  |
| adaptive 10→200µs | ~4.4k (**13×** fewer) | 10.35 ms (+0.9%) | 107 µs |
| adaptive 10→1ms   | ~0.96k (**62×** fewer) | 10.37 ms (+1.1%) | 464 µs |

The clustered makespan is essentially unchanged because the workload keeps the stealers finding
work, so they never leave `recheckMin` while load is present — the backoff is invisible to sustained
imbalance. The cost is paid only on the **first** pickup after a core has been idle long enough to
coast to the cap: that pickup waits up to ~`recheckMax`, then the successful steal snaps the stealer
back to aggressive. `10→200µs` is the recommended default — an order-of-magnitude cut in idle
wakeups for a sub-percent makespan cost and a ~100µs worst-case cold pickup.

This is the measured gate for the cross-thread steal-wake (#2). Backoff alone holds the sustained
clustered tail with no wake protocol, so #2 is **not** needed for throughput/makespan workloads. The
one case it does not cover is cold-pickup latency: a workload that is latency-sensitive *and* bursty
after idle pays up to `recheckMax` on the first unit of a burst. That — and only that — is the
measured case that justifies the ~2.3µs `MSG_RING` steal-wake, which would replace the cold-pickup
wait with the OS thread-wake floor. Absent that requirement, the cap is the cheaper instrument.
