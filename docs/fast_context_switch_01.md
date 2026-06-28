# Fast Context Switch and Direct-Yield Fastpath

A cooperative runtime's cheapest unit of work is the context switch, so it is the floor under every
other number. coop's `Context::Yield()` cost was measured against a peer stackful coroutine
runtime's equivalent (N contexts on one thread round-robining on the cheapest yield each runtime
offers, 1e8 switches, calibration-subtracted): coop ~35 ns/switch against the peer's ~9 ns. The gap
was not the io_uring layer — profiling put it squarely in the per-resume scheduler machinery. This
note describes the three changes that closed it and the one covenant the fastest of them must not
break.

## Where 35 ns went

A profile of the bare round-robin yield attributed the time to, in order: the per-resume cycle
accounting in `HandleCooperatorResumption`, `Context::Yield` itself, the cooperator loop, the bare
register save/restore, and — only ~5% — the io_uring poll. The poll was a red herring; the cost was
the scheduler doing work on every hop that a yield does not strictly need.

Three levers, each independent:

## 1. Gate the per-resume cycle accounting

`HandleCooperatorResumption` ran an unconditional `rdtsc()` on every resume to charge elapsed cycles
to the running context's `m_statistics.ticks`. That timestamp read was the single largest cost on the
bare yield path, and it was not gated by anything — production paid it on every switch.

The ticks it feeds are pure observability: only the status server's per-context view and the debug
context-tree print consume them; nothing on a scheduling or correctness path does. So the accounting
(and the `rdtsc` it needs) now sits behind `CooperatorConfiguration::trackContextCycles`, off by
default. The dashboard's per-context cycle column becomes an opt-in; the default yield stops paying
for a number it was not reading.

Effect on the microbench: ~35 → ~26 ns/switch.

## 2. Register-aware switch (clobber-delegated callee saves)

The x86-64 switch core saved and restored all six callee-saved registers (rbp, rbx, r12–r15) on
every switch, unconditionally. But most switches do not have all six live across them — saving a
register the caller was not using is wasted memory traffic.

The fix is the idiom the peer runtime uses. The asm core (`_coop_switch_context`) now saves only
`%rbp` and swaps the stack. The other callee-saved registers are listed in the **clobber list** of
the inline-asm wrapper at the call site (`ContextSwitch` in `context_switch.h`). The compiler, told
those registers are destroyed by the switch, spills and reloads only the ones actually live across
that particular switch — instead of the core blindly saving all of them. `%rbp` is special: under
`-fno-omit-frame-pointer` it is the frame pointer and must survive the call, so the core preserves it
directly; it also carries the new context's `Context*` into the trampoline on first entry, which is
why `ContextInit`'s fresh-stack layout collapses to three slots.

aarch64 deliberately keeps the all-callee-saved core, with a TODO. The same idiom applies there, but
writing and validating that asm needs an aarch64 host; an untested switch is a crash risk, so the
aarch64 core stays the known-correct version and its call-site wrapper is a plain call (correct
precisely because the core preserves everything).

Effect on the microbench: ~flat in isolation. The switch helpers (`YieldFrom`, `Resume`) are
standalone functions, not inlined into the scheduler, so the clobber list forces them to preserve
all callee-saved registers in their own prologue regardless, and the core's lone `%rbp` save is now
redundant with the frame-pointer push. The leaner core is the foundation the next lever needs — the
win lands once the hot switch sits in a tight frame.

## 3. Direct context-to-context yield (opt-in)

A plain `Context::Yield` trampolines through the cooperator loop: switch to the loop, run
`HandleCooperatorResumption` and the loop's bookkeeping, switch into the next runnable context. Two
switches and a scheduler pass for one logical hop.

The fastpath (`CooperatorConfiguration::directYield`) collapses that. When a yield finds another
runnable context, it switches **straight into it**: the yielding context takes the slot the loop
would have given it on the runnable list, and the next context is resumed in a single switch. This is
the same shape as the existing `Unblock` direct switch, applied to the cooperative-yield path; the
cooperator's saved frame is untouched, so control returns to the loop intact whenever the chain ends.

Effect on the microbench: ~26 → ~5.4 ns/switch at the default budget of 64 — past the peer's ~9 ns.
The default path (fastpath off) is byte-for-byte unchanged; a non-participant pays nothing.

It lands off by default. A change motivated by a peer comparison lands disabled until the win is
proven across regimes (the project's standing methodology); the microbench is one regime.

## Covenants

Negative-first — what these changes must not do.

- **The direct-yield fastpath must not starve io_uring.** The cooperator loop is the only place CQEs
  are reaped, and CQE processing between resumes is what unblocks blocking IO, fires timers, and
  releases the `Handle::Flash` barriers that run during context teardown. A chain of direct yields
  that never returns to the loop would deadlock any of those. The chain is therefore **bounded**: a
  per-cooperator budget refills each time the loop resumes a context and decrements on each direct
  yield; when it reaches zero, a yield falls back through the loop, which polls. Poll latency is
  bounded by `directYieldBudget` switches no matter how tightly contexts yield among themselves.
  `tests/test_direct_yield.cpp` pins this: spinners that do nothing but yield must not prevent a timer
  from firing or a killed in-flight `Recv` from tearing down. The test is red/green calibrated — with
  the bound defeated it hangs; with it in place the timer fires on time.

- **The default yield path must not change when the fastpath is off.** `directYield` defaults false;
  a cooperator that does not opt in runs the original trampoline-through-the-loop yield, unmodified.
  The fastpath is a branch the default path short-circuits, not a rewrite of it.

- **Cleanup paths must not depend on the fastpath.** Teardown (`Handle::Flash`, kill propagation,
  context exit) routes through `Block`/exit switches to the cooperator, never the direct-yield path,
  and works identically with the fastpath on or off. The full suite passes with `directYield` forced
  on as the global default at budgets 64, 2, and 1.

- **Cycle accounting must stay observability-only.** Gating `trackContextCycles` off must never change
  scheduling or correctness, because nothing on those paths reads `m_statistics.ticks`. The only
  visible effect of the default-off state is that the status dashboard's per-context cycle column
  reads zero until a cooperator opts in.

- **aarch64 correctness must not regress for an unproven optimization.** The register-aware switch is
  x86-64 only. aarch64 keeps the all-callee-saved core until the clobber-delegated form can be
  written and validated on an aarch64 host. Both architectures build; aarch64 stays correct.

## Numbers

Peer-facsimile thread-switch microbench, N=10 contexts, one cooperator, pinned clean core,
`COOP_PERF_MODE=2`, min-of-reps:

| Configuration | ns/switch |
|---|--:|
| baseline | ~35 |
| + gated cycle accounting (lever 1) | ~26 |
| + register-aware switch (lever 2) | ~26 |
| + direct yield, budget 64 (lever 3, opt-in) | **~5.4** |
| peer stackful runtime | ~9 |

Levers 1 and 2 ship as the default (1 changes a default-off observability knob; 2 is a pure
primitive swap). Lever 3 ships opt-in. With it enabled, coop's cheapest context switch is faster than
the peer's.
