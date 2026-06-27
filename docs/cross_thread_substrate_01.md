# Cross-Thread Work Substrate 01 (sketch)

Status: **sketch / RFC** — direction and open questions, not a committed design yet.

## Where we are

Single-cooperator continuations are done and proven end-to-end (see the continuation
commit arc `032bd11..87fa7ea`):

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

The a stackful runtime comparison and our own balancing experiments (see `[[coop-balancing-findings]]`,
`[[comparison-notes]]`) landed on a clear bet: for irregular compute, **work-queue
(morsel) balancing beats fiber/context migration**, and a **single global queue
negative-scales** — it must be sharded. a stackful runtime migrates stackful fibers; we intend to shed
cheap run-to-completion *morsels* and never migrate the execution context at all.

## The bet

> Continuations stay in-cooperator. To cross cores, a continuation *body* sheds a separate
> **task** into a sharded, work-stealing pool. The continuation node never crosses; only a
> task (a copy of the work) does.

This keeps the in-cooperator fast path untouched and confines all cross-thread atomics to
the substrate. The substrate is the morsel-sharing layer the balancing findings asked for,
and the thing we measure against a stackful runtime.

## Approach (proposed)

1. **Per-cooperator shard, work-stealing deque.** Each cooperator owns one Chase-Lev-style
   deque. The owner pushes/pops its own end LIFO (recently-shed work is cache-hot); idle
   thieves steal the far end FIFO (oldest task, likely the largest remaining subtree).
   This is the structure Go/Rayon/a stackful runtime all converged on; no reason to be cleverer first.

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

## Open questions (want a steer before committing)

- **Replace or sit beside `Submit`?** Lean: sit beside. `Submit` = targeted ("that
  cooperator"); `Shed` = balanced ("anywhere"). Different intents; collapsing them loses the
  affinity case.
- **Shard granularity:** per-cooperator (simplest, matches core pinning) vs per-NUMA-node
  (fewer shards, less stealing, but coarser balance). Lean per-cooperator with NUMA-aware
  steal order.
- **Task representation:** a pooled closure node (like the continuation pool, but
  cross-thread-owned) vs a `Launchable`. The continuation pool is single-cooperator/no-atomic
  and cannot back cross-thread tasks; the task pool needs its own allocation story (likely a
  per-cooperator slab freed by whoever runs the task — cross-thread free).
- **Fairness vs locality knob:** pure LIFO-local can starve old tasks under a steady local
  producer; how aggressively do thieves intervene? Start with classic Chase-Lev and measure.

## Phasing

- **v1:** per-cooperator Chase-Lev deque, plain atomics, random-victim stealing, `Shed`
  API, wake-on-shed. Benchmark vs `Submit` and vs the static-shard balancing skeletons
  from `[[coop-balancing-findings]]`.
- **v2:** NUMA-aware steal order.
- **v3:** rseq fast path for the owner's local end.

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

## Validation

- Microbench: `Shed`+steal round-trip vs `Submit` round-trip (latency of crossing cores).
- Scaling: the irregular-compute morsel workload from the balancing experiments, coop
  work-stealing pool vs static sharding vs a stackful runtime — items/s across core counts, and tail
  latency (the irregular case is where stealing should win).
- Apples-to-apples vs a stackful runtime on the original morsel workload that motivated the comparison.
