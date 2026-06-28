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

A Continuation waits on a `Coordinator` in place of a blocked `Context` — the wait-list node carries
either a `Context*` or a `Continuation*`, distinguished by a tag bit. When the coordinator is
Released, instead of waking a context (a full context switch), the continuation **Runs as a function
call** on the releasing cooperator. "Migrate the closure, not the stack."

**Loop-drained dispatch.** Firing is not inline at `Release`; the continuation is enqueued to a
per-cooperator pending list that the scheduler drains after every io_uring poll. Draining to empty
iteratively bounds native stack depth no matter how deep a continuation chain runs, batches bursts
for icache locality, and — crucially — makes firing **context-free**, which is what lets a
continuation fire straight from an io_uring CQE: `Handle::Finalize` already does
`coord.Release(ctx, false)`, so a continuation registered on an IO handle's coordinator fires from
the completion with zero added plumbing.

**Two flavors, one spectrum:**

- **Structured** (`coord.Continue(fn)`) — frame-hosted via guaranteed copy elision (zero heap),
  with a single-waiter completion latch so the registrant can `Await()` a typed result in place. It
  is RAII-safe: unfired ⇒ cancelled on scope exit, so it cannot leak. Keeps **one** parked context
  (the awaiter) per in-flight pipeline.
- **Detached** (`coord.ContinueDetached(fn)`) — pooled, self-freeing, no awaiter and **no parked
  context**. `fn` is terminal; a pipeline continues by registering the next detached continuation.
  This is the high-fan-out form.

**The completion latch.** A structured continuation has at most one awaiter, so signalling it does
not need a full `Coordinator` (FIFO wait list, held/release bookkeeping). It uses a slim
single-slot latch — a parked-context pointer plus a fired bit — which brought structured dispatch
down to match the detached form (~8 ns) while keeping zero-alloc, RAII, and a returned result.

**Allocation.** Detached continuations are backed by a per-cooperator free-list pool. Because they
are single-cooperator (allocated, fired, and freed on one cooperator's thread), the pool needs no
atomics; it eliminated the per-fire `malloc`/`free`.

**Lifetime.** A continuation's lifetime is owner-managed, exactly like a blocked context: whoever
registers it guarantees it fires or is cancelled before the coordinator dies (for IO, the Handle's
fire-or-cancel teardown does this). A debug-only assert at coordinator destruction catches a leaked
waiter at the bug site.

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
went from being outperformed to outperforming by a meaningful margin — and we believe the gap is
**structural**, a consequence of the tradeoffs any runtime must make to support stackful work
migration. The measured clustered makespan (6 cores, lower is better; 35 ms is the perfect-balance
floor):

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
