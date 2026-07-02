# Documentation Index

## Start Here

1. `AGENTS.md`: quick operating guide for coding agents
2. `README.md`: project overview and build/test quick start
3. `CONTRIBUTING.md`: development workflow and expectations

## Architecture and Internals

- `CLAUDE.md`: top-level architecture map
- `coop/CLAUDE.md`: scheduler, context lifecycle, and internals
- `coop/io/CLAUDE.md`: io_uring layer and IO operations
- `coop/io/ssl/CLAUDE.md`: TLS and BIO modes
- `coop/http/CLAUDE.md`: HTTP implementation and routing
- `coop/perf/CLAUDE.md`: performance counter and sampling internals
- `tests/CLAUDE.md`: test harness behavior and pitfalls

## Design Guidance

- `DESIGN_IDIOMS.md`: API and performance design principles
- `blocking_io_shutdown_01.md`: blocking-IO shutdown contract and the explicit
  kill-aware composition pattern for coop-owned loops
- `continuations_01.md`: the continuation primitive (stackless run-to-completion
  units) and the performance argument for stackless work-stealing vs the fully-
  stackful-fiber model
- `cross_thread_substrate_01.md`: the sharded work-stealing substrate (Grid, deque,
  covenants, calibration) that continuation bodies shed morsels into
- `fast_context_switch_01.md`: the three levers that cut `Context::Yield` cost to peer
  parity and past it — gating the per-resume cycle-accounting rdtsc, the register-aware
  (clobber-delegated) x86-64 switch core (since superseded — see `context_switch_core_01.md`),
  and the opt-in direct context-to-context yield fastpath, with the negative covenant that the
  fastpath must never starve io_uring (it is bounded by a budget that forces a polling fallback
  through the cooperator loop)
- `context_switch_core_01.md`: why the switch core saves all callee-saved registers behind a
  plain, compiler-visible call — the published register-save-minimization alternative (Photon's
  CACS) evaluated, shipped, and withdrawn on evidence: hidden-call red-zone crash at `-O2`,
  measured perf-neutrality once the mitigation cost is included, and an honest accounting of a
  misattributed LTO episode; with the negative covenants that no inline asm may hide the switch
  call and no TU may need a correctness-bearing codegen flag
- `timer_slack_01.md`: opt-in deadline quantization on the `Sleep` path — collapses
  per-timer kernel wakeups for a fan-out of concurrent sleeps, with the negative
  covenant that correctness deadlines stay exact
- `timer_wheel_001.md`: per-cooperator userspace timer queue (intrusive pairing heap)
  that arms a single io_uring timeout for the nearest deadline and services many sleeps
  per wakeup — cuts kernel hrtimer churn from O(N) per sleep to O(1) per cooperator,
  with the negative covenants that the structure stays thread-local, a sleep never
  fires early, and IO timeouts stay exact and uncoalesced
- `buffer_ring_multishot_01.md`: provided buffer ring + multishot recv — decouples
  resident recv memory from connection count for keep-alive fan-out and removes the
  per-message recv submission, with measured throughput and memory and the honest
  push/pull impedance between kernel-push completions and coop's pull consumers
- `buffer_ring_multishot_02.md`: cross-context delivery design (#16) — detached-continuation
  delivery of a connection's multishot completions to a different (co-resident) consumer
  context, with the buffer pool as the sole back-pressure; the deferred piece from _01
- `TODO.md`: prioritized follow-up work
