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
- `timer_slack_01.md`: opt-in deadline quantization on the `Sleep` path — collapses
  per-timer kernel wakeups for a fan-out of concurrent sleeps, with the negative
  covenant that correctness deadlines stay exact
- `buffer_ring_multishot_01.md`: provided buffer ring + multishot recv — decouples
  resident recv memory from connection count for keep-alive fan-out and removes the
  per-message recv submission, with measured throughput and memory and the honest
  push/pull impedance between kernel-push completions and coop's pull consumers
- `TODO.md`: prioritized follow-up work
