# Context Switch Core: Save-All Through a Real Call

The context switch is coop's most fundamental primitive: every yield, block, resume, and IO wait
crosses `_coop_switch_context`. This document records why the switch core saves **every**
callee-saved register and is reached only through a **plain, compiler-visible function call** —
deliberately rejecting the register-save-minimization technique published for the Photon runtime,
after implementing it, shipping it, and measuring what it actually cost here.

## The design space

A stackful switch must preserve whatever the ABI says survives a function call, then swap stack
pointers. There are two places that preservation can live:

**Save-all, out-of-line (boost.context's `jump_fcontext`, coop's aarch64 core since inception).**
The core is a real assembly function that saves all callee-saved registers unconditionally. The
call site is an ordinary call; the compiler sees it, models it, and arranges the caller's state
per the ABI. The cost critique: most switches have few callee-saved registers live, so most of
those saves move dead values.

**Clobber-delegation (Photon's CACS — "context-aware context switching").** Photon's paper,
*Stackful Coroutine Made Fast* (Li, Du, Lin, Hsu — Alibaba Cloud; self-published at
photonlibos.github.io/blog/stackful-coroutine-made-fast; the same idea appears earlier in Rust's
libfringe), argues the compiler should decide what to save at each call site: the core persists
only the frame pointer and stack pointer, the `call` into it is issued **inside an `asm volatile`
block**, and the remaining callee-saved registers are named in that block's clobber list. The
compiler then spills exactly the registers live across that particular switch. The paper reports
switch cost "similar to that of an ordinary function call."

coop's x86-64 core used the CACS form; the aarch64 core was always save-all. Both cores are now
save-all, and the wrapper is a plain call on every architecture.

## Why CACS lost

The saving CACS promises is real only if the hidden `call` is harmless. It is not — hiding the
call from the compiler is not an implementation detail of the technique, it **is** the technique
(a visible call would reinstate the full ABI contract CACS exists to avoid). Everything below
follows from that one property.

**1. The red zone (latent crash, observed at `-O2`).** The x86-64 psABI's red zone is sound only
for code the compiler can prove makes no calls; a `call` pushes a return address below RSP. With
the call hidden inside inline asm, GCC's red-zone analysis saw a leaf region, spilled a live
`this` into the red zone across the switch, and the switch clobbered it — SIGSEGV, at `-O2` only,
because other optimization levels happened to allocate real frames. This is a documented GCC
limitation, not a bug: the `"memory"` clobber covers program-visible memory, explicitly not the
compiler's spill slots, and GCC's maintainers' guidance for calls inside asm is "compile with
`-mno-red-zone`". (GCC 15 adds a first-class `"redzone"` clobber for exactly this pattern —
an admission the prior interface could not express it.)

**2. LTO (an honest accounting).** The suite long failed catastrophically under `-flto` (0 of 15
runs clean), and the failures survived every red-zone flag — which for a time read as "the hidden
control transfer is LTO-incompatible." Chasing that conclusion down falsified it: the crashes were
a separate scheduler-lifecycle bug (a stale `thread_cooperator` binding misrouting cross-thread
kills — see `Cooperator::Launch`), which LTO's more uniform stack layouts merely made
deterministic. With that bug fixed, the full suite passes under LTO with either switch design. What
stands is the modeling asymmetry, not an observed miscompile: an opaque asm block hiding a control
transfer is invisible to whole-program codegen *by construction*, so its LTO safety is an absence
of failure, where a real call's safety is a modeled guarantee. The episode cuts both ways and is
recorded as such — the hidden call was never shown to break LTO, and a hidden control transfer was
a plausible enough suspect to absorb a week of misattribution that a visible call would never have
invited.

**3. The measured benefit is nil once the mitigation is priced in.** The mandatory
`-mno-red-zone` on the switch-emitting TU forces frame allocation in functions that would
otherwise use the red zone, costing ~13% on the barest switch microbenchmark. The save-all core
adds five push/pop pairs of L1-hot, off-critical-path traffic. Measured head to head on the same
machine, the two complete configurations land within noise of each other (~27ns on the pure
yield loop; zero difference on workload-shaped benchmarks, where the switch is a sliver). CACS's
win and its mitigation cost cancel. The paper's own §3.3 points the same direction: it names a
custom calling convention (`preserve_none`) as the optimal end-state — first-class compiler
support, not a hidden call.

**4. Everything else that models calls.** Sanitizer fiber annotations, CET shadow stacks,
profiling instrumentation, and PGO all reason about visible control transfers. A real call gets
them for free or nearly so; an opaque asm block fights each one separately, forever.

The trade, stated honestly: save-all through a real call costs nothing measurable and buys a
switch the entire toolchain understands. Clobber-delegation is a bet that hiding control flow
from the compiler stays cheap across every optimization level, linker mode, and future codegen
feature — a bet that lost outright once (`-O2` red zone) and cost a week of misattributed LTO
debugging before it was withdrawn.

## The shape that remains

- `context_switch.S` — both cores save all callee-saved registers and swap the stack pointer.
  x86-64: rbp, rbx, r12–r15. aarch64: x19–x28, fp, lr.
- `context_switch.h` — `ContextSwitch` is `return _coop_switch_context(...)` on every
  architecture. No inline asm at the call site.
- `ContextInit` seeds a fresh stack with the core's exact save-area layout, so the first switch
  into a context is indistinguishable from a resume.
- No translation unit carries `-mno-red-zone` or any other correctness-bearing codegen flag.

Re-verification, should the model ever be touched: build and run the full suite at `-O2`
(unflagged — this is the configuration the hidden call crashed) and under `-flto`. Both must
pass; the CI matrix runs both lanes. The CACS-era `-O2` failure signature was a red-zone spill
of a live local (`this`) across `Resume`, reloaded as garbage after the switch.

## Covenants

- The switch core is reached **only** through a plain, compiler-visible call. No inline-asm
  block may contain a `call`/`bl` to the core or otherwise hide a control transfer from the
  compiler.
- No TU may require a codegen-restricting compile flag (`-mno-red-zone` or kin) for
  correctness. A change that reintroduces such a requirement is wrong, whatever it buys.
- The core preserves every ABI callee-saved register on every architecture, and `ContextInit`'s
  seeded layout matches the core's save area slot for slot. A mismatch is UB that a green suite
  at one optimization level does not rule out.
- Both architectures share one model. An arch-specific register-saving "optimization" that forks
  the model requires revising this document first — and must bring (a) a workload-level measured
  win, not a bare-switch delta, and (b) a toolchain story covering the red zone **and** LTO
  (GCC 15's `"redzone"` clobber answers only the former).
