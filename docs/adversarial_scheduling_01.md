# Seeded and Adversarial Scheduling

A cooperative runtime decides, at every suspension point, which context runs next. That decision is
the whole of a program's concurrency: state is only ever observable by another context across a
suspension, so the set of interleavings a program can exhibit is exactly the set of choice sequences
its scheduler can make.

coop's default choice is a FIFO pop. The runnable list is a queue, a suspending context goes on the
tail, the head is resumed. That is strict round-robin, and it means the reachable set has **one**
member. The same rotation, every run, forever.

## Why that is a problem

The dominant defect shape in a cooperative codebase is a multi-step mutation of shared state with a
suspension in the middle — publish a pointer, yield, publish its length; bump a counter, wait on IO,
bump its partner. The violation is *continuous*: the state is inconsistent for the whole span
between the two steps, on every single execution. Only its observability is contingent, and what it
is contingent on is whether the scheduler happened to run the context that looks.

Round-robin does answer part of that: every context that was runnable when the window opened does
run before the yielder resumes, so a single window with a single observer is inspected reliably.
What round-robin cannot do is vary. The *order* in which two observers enter the window is fixed by
the rotation. Whether a third context gets between a coordinator's release and its waiter is fixed
at "no". A defect that needs the other ordering is not rare under round-robin — it is unreachable,
which from the outside is indistinguishable from a defect that does not exist.

Seeded scheduling makes the choice sequence a parameter instead of a constant, and makes it
reproducible so that a failure found once can be run again.

## The mode

Two knobs on `CooperatorConfiguration`, both off by default:

```cpp
CooperatorConfiguration cfg = s_defaultCooperatorConfiguration;
cfg.schedulingMode = SchedulingMode::Seeded;
cfg.yieldPolicy    = YieldPolicy::Adversarial;
cfg.schedulingSeed = 20260823;
```

`SchedulingMode::Seeded` routes context selection through a per-cooperator splitmix64 stream seeded
from `schedulingSeed`, so the schedule becomes a pure function of the seed and the selections made so
far. `YieldPolicy` picks what that stream is used for:

- **`Fifo`** keeps round-robin order. The mode's plumbing is live and the seed is reported, but no
  scheduling decision changes. It is the control arm — it is what makes "the mode itself perturbs
  nothing" a testable claim.
- **`Adversarial`** draws uniformly from the runnable set, skipping past whichever context the policy
  picked last so a suspending context never immediately gets itself back, and additionally routes a
  *scheduled wake* through the runnable queue rather than handing control straight to the woken
  waiter.

That second half matters more than its size suggests. `Coordinator::Release(ctx)` normally switches
from the releaser directly into its waiter, which means no third context ever runs in the interval
between them — and that interval is precisely where a mutation guarded by that coordinator is
half-applied. It is the runtime's single largest scheduling blind spot, and it is not a narrow one:
it is on every contended lock, every signal, every cooperative handoff. The adversarial policy
removes it, and removes it into a path the runtime already takes, since a CQE-driven release wakes
through the queue anyway.

## Replay

A schedule that finds a failure is worthless if it cannot be run again, and the run that finds it is
usually a suite run on a build host rather than something anyone is watching. So the mode is drivable
entirely from the environment of an already-built binary, and whenever it is on the resolved seed is
printed once on stderr together with the environment that reproduces it:

```
$ COOP_SCHED_POLICY=adversarial ./build/debug/bin/coop_tests
[coop] seeded scheduling active: policy=adversarial seed=6134401882954174321 (replay: COOP_SCHED_SEED=6134401882954174321 COOP_SCHED_POLICY=adversarial)
```

```
$ COOP_SCHED_SEED=6134401882954174321 COOP_SCHED_POLICY=adversarial ./build/debug/bin/coop_tests
```

`COOP_SCHED_SEED=<n>` selects seeded mode with that seed; `COOP_SCHED_SEED=random` derives one;
`COOP_SCHED_POLICY=adversarial|fifo` selects the policy and implies seeded mode. The environment
overrides configuration, so a program that never heard of the mode can still be driven by it.

The override is process-wide and unconditional, which is the point and also the one thing it costs:
code that configures a cooperator's policy and then asserts on the order it gets is, under a forced
environment, asserting something the environment has already taken away. Such a test must skip when
either variable is set rather than expect configuration to win.

## What determinism does and does not cover

Every cooperator in a process seeds its stream from the same value; their streams then diverge on
their own because their runnable sets do. A **single-cooperator** program is therefore fully
reproducible under a fixed seed: same seed, same interleaving, turn for turn.

A multi-cooperator program gets reproducible per-cooperator streams, but the interleaving *between*
cooperators stays the operating system's to decide. Neither does the mode touch the arrival order of
io_uring completions. Seeding removes the scheduler as a source of variation; it does not make a
multi-threaded, kernel-scheduled program deterministic, and nothing here should be read as claiming
otherwise.

## Cost when off

`schedulingMode` defaults to `Default`, and the mode is flattened at construction into two bools on
the `Cooperator`. Every site that takes a context off the runnable list goes through
`Cooperator::NextRunnable`, whose default arm is the `m_yielded.Pop()` it always was, behind one
`[[unlikely]]` test of an already-hot field. The seeded body is out of line, so the inline cost is
the pop plus a never-taken branch: no allocation, no atomic, no syscall, and the PRNG state is not
touched. The wake path is the same shape — one bool test in `Cooperator::Unblock`.

The selection arithmetic is plain 64-bit integer work with no atomics and no intrinsics, so it needs
nothing architecture-specific on either x86-64 or aarch64.

With the mode *on*, selection is O(runnable) rather than O(1): the runnable list is intrusive and
singly-walkable, so drawing the k-th element means measuring the list and then walking it. That is
paid deliberately -- a mode whose entire purpose is to slow a program down into its unlikely
interleavings has no business optimizing its own selection -- but it does mean a run under the
adversarial policy is not a run whose timings mean anything.

## Reading a failure under the policy

A failure that appears only under the adversarial policy is one of three things, and telling them
apart is the first step of any triage.

**The test asserts the default policy.** A test that releases a coordinator with `schedule` set and
then immediately asserts the waiter has already run is asserting the handoff, which this policy
removes on purpose. It is not a defect and the test is not wrong -- it is pinning the default
behaviour, which is exactly what it should do. coop's own suite has several: the assertions that
follow `Release(ctx, true)` in the coordinator tests read the state the handoff produces.

**The test assumes an ordering the runtime does not promise.** The commonest form: suspend once and
assume some queued work has run by the time control comes back. The scheduler drains queued
continuations on its way round the loop, so under round-robin one yield usually is enough -- but
"usually, under one rotation" is not a guarantee, and the loop has paths that resume a context with
continuations still queued. This shape is worth taking seriously even though the symptom is a test:
the same assumption in non-test code is a latent use-after-free, because a continuation that fires
after its registrar's frame is gone reads freed memory (see `Cooperator::DrainContinuations`).

**A real defect.** The window was always open; nothing was looking into it.

The mode cannot make this call for you, so triage starts by re-running under
`COOP_SCHED_POLICY=fifo`. That keeps the seeded machinery live while restoring the default order: a
failure that survives `fifo` is not about ordering at all, and one that disappears is one of the
three above.

## Covenants

- **The default path is not a variant of the mode.** With `schedulingMode` at `Default`, selection is
  the same list pop, in the same order, at the same cost. No configuration of the mode may be
  reachable without an explicit opt-in, by configuration or environment.
- **The mode is not a production feature.** Nothing outside a test harness may depend on seeded
  selection for correctness, ordering, or fairness. A defect that only reproduces with the mode on is
  still a defect in the code under it, never a defect in the mode.
- **A seed is never silent.** Any run with the mode on must print a seed that reproduces it. A
  derived seed that is not reported is the same as no seeded mode at all.
- **Adversarial selection stays a schedule.** It reorders, it does not starve: every runnable context
  is still selected, and no context may be indefinitely passed over.
- **Failures found under the mode are not fixed by the mode.** The policy exposes pre-existing
  windows; widening or narrowing the policy to make a failure go away inverts the entire point.
