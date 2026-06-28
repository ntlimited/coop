# Continuations and Stackless Work Units

How coop decomposes IO-driven work and spreads it across cores without paying a stack per unit.
For the cross-core work-sharing design specifically (covenants, calibration), see
`cross_thread_substrate_01.md`; this document is the continuation primitive and the performance
argument behind it.

## The problem

coop's unit of execution is the `Context` — a stackful coroutine with its own segment (16KB by
default). That is the right unit for a connection handler or any code with real sequential control
flow that must suspend with its stack intact. It is the wrong unit for two things:

1. **Fine-grained IO decomposition.** An IO-driven pipeline — recv → parse → route → respond, each
   stage waiting on a completion — wants to be a chain of cheap stages, not a parked 16KB stack per
   in-flight pipeline.
2. **High fan-out.** Tens of thousands of concurrent in-flight operations want to cost tens of
   bytes each, not 16KB each, and want to advance without a context switch per stage.

Continuations are the answer to both: stackless, run-to-completion units that cost ~8–31 ns to
dispatch and carry no stack, so a stage costs a function call instead of a context switch and an
in-flight pipeline costs a small heap node instead of a parked stack.

## Thunk: the shared shape

Both stackless units are a `coop::Thunk` — a type-erased nullary action with `virtual void Run()`
that runs start-to-finish on whatever context invokes it and **never suspends** mid-execution. Two
species split on the one axis that is load-bearing — *does it cross cores*:

| | Continuation | Erg |
|---|---|---|
| triggered by | a Coordinator release (an *event*) | a stealer pulling it (a *worker*) |
| domain | **in-cooperator, never migrates** | **crosses cores** |
| cost model | single-cooperator, **no atomics** | cross-thread, atomic alloc/steal |

They compose along that axis: an Erg is the cross-core seed; once it lands on a cooperator and runs,
it decomposes its async follow-on into Continuations. Keeping the names distinct keeps the migrate /
no-migrate boundary legible — "can I shed a Continuation?" and "can I await an Erg?" answer
themselves.

## Continuations (the in-cooperator species)

A continuation waits on a `Coordinator` in place of a blocked `Context`. When the coordinator is
released, instead of waking a parked context — a full context switch — the continuation **runs as a
function call** on the cooperator that did the release. The slogan: migrate the closure, not the
stack.

The case this is built for is an **IO completion.** A pipeline stage's natural shape is "do a little
work, issue an async operation, continue when it finishes." In a stackful world, "continue when it
finishes" means a context is parked on that operation and woken — a context switch per stage and a
16 KB stack held idle for the whole wait. We would rather the completion *itself* run the next stage,
directly, as a callback: no parked stack, no switch. That is exactly what a continuation registered
on the operation's coordinator does, and because coop's IO already signals completion by releasing
that coordinator, the next stage fires straight off the io_uring completion with nothing added to the
IO path. This is the reason continuations exist; everything below serves it.

For a completion to run the next stage safely, firing must happen with **no current context** — a
completion is handled by the cooperator's scheduler loop, not by any running context. So firing is
not inline at `release`: the continuation is enqueued onto a per-cooperator pending list that the
loop drains after it polls io_uring. That context-free drain is the load-bearing property. Draining
iteratively to empty also bounds native stack depth no matter how long a continuation chain runs, and
batches a burst of completions for instruction-cache locality — both fall out for free.

**The drain is bounded in length, not just stack depth.** Draining to empty is safe only because the
usual continuation chain returns to io_uring between stages: a stage issues an async op and the next
stage waits on its CQE, so the chain is paced by completions and the drain empties as soon as the
current burst is consumed. A chain whose next stage is *always ready* — a stage that hands off
synchronously through an already-released coordinator (buffered data, an internal producer/consumer
rendezvous) rather than waiting on a CQE — breaks that assumption: it re-arms and fires within the
same drain, so an unbounded drain would run it to its natural end before the loop could poll again,
delaying the pickup of every unrelated IO completion sitting in the CQ behind it. This is the
in-cooperator analogue of reactor starvation.

The drain is paced against the real question — *is IO actually waiting?* — rather than a guessed
count. Every `Cooperator::m_drainPeekStride` continuations it peeks io_uring
(`Uring::HasPendingCompletions()`, a userspace read of the CQ head/tail and the COOP_TASKRUN flag, no
syscall) and breaks the instant completions are pending, so a CQE sitting behind the chain is
harvested within a stride. When nothing is pending the peek costs nothing observable and the chain
runs on, so a purely synchronous chain with no concurrent IO pays no spurious breaks. A coarse op
budget (`Cooperator::m_drainBudget`) backs the peek as a hard ceiling that bounds native stack depth
and caps a pathological chain that somehow never lets a completion land. The budget alone is a blunt
proxy for IO readiness — too low wastes polls on a quiet chain, too high lets a CQE languish; the
peek supplies the precise signal and the budget only guarantees an upper bound.

The break is only half the latency story: the scheduler's idle branch, when a poll completes IO that
wakes a context, schedules that context *before* re-draining the continuation backlog. The
freshly-completed IO is exactly the work that was queued behind the chain; re-draining to the budget
ceiling first would reintroduce the very pickup tail the peek exists to cut. The queued continuations
are not lost — they ride the next iteration's drain, interleaved with the woken context. Together the
peek and the ordering convert "drain bounds stack depth" into "drain bounds completion-pickup
latency" while keeping a high ceiling: at a fixed budget the peek cuts the synchronous-chain p99
completion-pickup tail by roughly an order of magnitude with no throughput cost on a quiet chain.
Stride 0 disables the peek (budget-only); budget 0 disables the ceiling (peek-only); both 0 drains
strictly to empty.

**Two flavors, one spectrum:**

- **Structured** (`coord.Continue(fn)`) — frame-hosted via guaranteed copy elision (no heap), with a
  single-waiter completion latch so the code that registered it can `Await()` a typed result in
  place. RAII-safe: if it never fires it is cancelled on scope exit, so it cannot leak. Costs one
  parked context — the awaiter — per in-flight pipeline.
- **Detached** (`coord.ContinueDetached(fn)`) — pooled, self-freeing, with no awaiter and **no parked
  context at all**. `fn` is terminal; a pipeline continues by registering the next detached
  continuation. This is the high-fan-out form.

**The completion latch.** A structured continuation has at most one awaiter, so the thing that signals
"done" to that awaiter does not need a full `Coordinator` — no FIFO wait list, no held/release
bookkeeping. It is a single slot: a parked-context pointer and a fired bit. That is why structured
dispatch is as cheap as the detached form (~8 ns) without giving up the zero-heap, RAII, or returned
result.

**Allocation.** Detached continuations come from a per-cooperator free-list. Because a continuation is
allocated, fired, and freed on a single cooperator's thread, that pool needs no atomics and no
`malloc` on the firing path.

**Lifetime.** A continuation is owner-managed, exactly like a blocked context: whoever registers it is
responsible for it firing or being cancelled before the coordinator it waits on is destroyed (for IO,
the Handle's teardown cancels in-flight work). A debug-only assert at coordinator destruction catches
a leaked waiter at the bug site.

## Work-sharing (the cross-core species)

An Erg is shed into a `work::Grid` — an opt-in domain of cooperators that balance Ergs by stealing.
Each participant owns a bounded Chase-Lev deque and runs a daemon stealer that pulls local work,
steals from peers, runs each Erg to completion, and parks on a short io_uring timer when idle.
`Shed(fn)` is the verb, sibling to `Spawn`: with a Grid it sheds a balanced Erg; without one it falls
back to `Spawn` (the "shed = spawn" default). Opt-in is free — a non-participant pays nothing. The
full design, covenants, and calibration live in `cross_thread_substrate_01.md`.

## Safety: the run-to-completion contract

A Thunk borrows a host context — the cooperator's green thread for a continuation drain, or a
stealer for an Erg — so suspending inside one (Yield, Block, or the blocking Coordinate/IO paths
that funnel through them) would stall the *host*, not the Thunk. A debug-only guard tracks "running
in a Thunk" and asserts at the suspend chokepoints, catching the misuse at its call site. It
compiles out entirely in release.

## Performance

All figures measured on one machine (Xeon 6975P-C), release build, warm.

### Per-unit dispatch cost — the cost of being suspendable

The unit hierarchy, at zero real work (pure dispatch):

| unit | ns/unit | suspends? | stack |
|------|---------|-----------|-------|
| bare worker-loop call | ~5 | ✗ | none |
| **coop continuation** | **~31** | ✓ | none (~128 B node) |
| coop context | ~93 | ✓ | 16 KB |
| stackful fiber (work-stealing runtime) | ~960 | ✓ | full, migratable |

The continuation is **the cheapest *suspendable* unit** — ~3× lighter than coop's own stackful
context and ~30× lighter than a stackful fiber. The bare call is cheaper still but cannot await IO,
so it only serves non-suspending compute morsels. The structured-continuation *dispatch* itself
(register → fire → collect) is ~8 ns, versus ~68 ns for a context handoff.

The shape matters more than the ratio. A stackful fiber's ~960 ns floor is paid *regardless of how
much real work the unit does*: a fiber doing 100 ns of work still costs ~960 ns, ~90% machinery.
That is the "don't spawn a fiber per morsel" trap. A continuation's ~31 ns floor makes fine-grained
decomposition actually viable.

### Memory at fan-out

An in-flight pipeline costs a continuation node (~128 B) instead of a parked context (16 KB) — a
**227× footprint reduction**. At 4096 concurrent pipelines that is ~295 KB resident versus ~67 MB of
stacks. And the per-pipeline advance cost stays flat where context-per-pipeline degrades under cache
pressure: a fan-out benchmark measured a 5.4× throughput advantage at 64 pipelines widening to 12.7×
at 4096, as the contexts' working set blows the cache while the continuation nodes stay resident.

### Stackless work-stealing vs. the fully-stackful model

A word on framing first, because the numbers below are easy to be smug about and smugness here would
be a mistake. We were glad to see a production-grade library appear in this space — built for an
exceedingly similar problem (network-bound, high-fan-out services) from an entirely different
foundational choice: stackful green-threads with work-stealing migration. We measured coop against it
both to verify coop's strengths for what it was built for and to learn what a mature library from a
different starting point does well, and we took plenty (cross-thread allocator techniques, timer and
sleep ideas, an escape hatch for blocking third-party code).

With that said: on a *synthetic micro-benchmark* of irregular, IO-suspending, multi-core work, coop
came out ahead of the stackful model — and the *why* matters more than the margin, because it is not
the flashy kind of result. We did not stumble onto a green-threading trick that suddenly beats other
libraries. We followed coop's architecture to its logical conclusion, and that architecture happens
to give a graceful **performance-for-complexity gradient**: stackful contexts for as long as their
control flow is worth the cost, and — once you reach the point where even a cheap context switch is
too much and you need a unit's dispatch down toward tens of nanoseconds — the stackless path as the
*next step on the same road*, not a different library or a rewrite. A runtime that commits every unit
to the stackful price (a stack each, ~1 µs to dispatch) has no cheap end of that gradient to step
onto; walking ours far enough is what comes out ahead. The number below is a consequence of that
gradient, not a margin we engineered for. The measured clustered makespan (6 cores, lower is better;
35 ms is the perfect-balance floor):

| approach | clustered makespan | of floor |
|----------|--------------------|----------|
| coop, static shared-nothing (no balancing) | ~150 ms | 23% |
| stackful-fiber work-stealing | ~74 ms | 47% |
| **coop's stackless unit + morsel pull** | **~44 ms** | **80%** |

Then the obligatory humility. There is a Mike Tyson line — *everybody has a plan until they get
punched in the mouth* — that applies with full force to anyone quoting their own synthetic benchmark,
and doubly so when the comparison is a research-grade harness against production-deployed code that
has actually met real load. These numbers say something real about the *architecture*; they do not
claim coop would win a production bake-off, and we are not claiming it would.

The structural point is what we stand behind. Two findings from the table:

1. **Balancing is essential, and a cheap unit alone cannot substitute for it.** coop's static-shard
   approach — even with the 31 ns unit — loses badly (~150 ms) when load clusters, because no amount
   of unit-cheapness moves work off an overloaded core. This is *why* the work-sharing layer exists.
2. **Given balancing, separating the unit from the migratable object pays off.** A stackful runtime
   fuses two things into one: the unit of execution and the thing work-stealing moves. So every unit
   pays the full stackful price (a stack, ~1 µs to dispatch) to be *stealable* — even when nothing
   needs stealing. coop separates them: the stackless unit is cheap (~31 ns) and never moves;
   balancing is a distinct layer that ships cheap morsels across cores. You pay for migration only
   where load actually clusters, and nothing with a stack ever moves — which also sidesteps the whole
   class of stack-local-state migration hazards.

### What this is (and is not)

It is worth being blunt about the tradeoff. coop is **not** trying to win by being a better stackful
green-threading library, and continuations are **not** a replacement for that model. coop's stackful
`Context` idioms — ordinary sequential control flow that suspends with its stack intact — are the
default, and they are *extremely* performant. The continuations here are a **stackless fastpath**:
they translate a context switch into a callback on a queue.

The value of that fastpath is bounded and specific. Stackful idioms carry you comfortably until you
are chasing microsecond-level latency; *then, and only then,* does it become worth reaching for the
Amdahl's-law junctures where a stack or a context switch is the dominant cost:

1. **Extreme swappiness** — very high wake/yield frequency over trivial per-unit work, where the
   context switch is most of the cost.
2. **Load-balancing junctures** — where cheap, migratable morsels rebalance irregular work across
   cores without moving a stack.

Outside those regimes the right answer in coop is still a `Context`. The fastpath is a scalpel, not a
new default.

## Where the numbers can still move

The clustered figure is gated by the stealer's idle recheck interval (a stealer parks on a short
io_uring timer and re-checks for stealable work); a cross-thread steal-wake would let that interval
be larger and tighten the tail further. That, plus chunked steals and a per-cooperator task slab,
are the optimization levers — see `cross_thread_substrate_01.md`.
