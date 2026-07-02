# Coop Codex Guide

This file is a fast operating guide for Codex-style agents. It complements, and does not replace, the `CLAUDE.md` files.

## Fast Start

From repo root:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
./build/debug/bin/coop_tests
```

Focused loop:

```bash
./build/debug/bin/coop_tests --gtest_filter='IoTest.*'
```

## Read Order

Use this order before editing non-trivial code:

1. `CLAUDE.md` (repo-wide architecture and conventions)
2. `coop/CLAUDE.md` (scheduler and context internals)
3. Relevant component guide:
   - `coop/io/CLAUDE.md`
   - `coop/http/CLAUDE.md`
   - `coop/perf/CLAUDE.md`
   - `tests/CLAUDE.md`
4. `DESIGN_IDIOMS.md` (API and data-layout principles)

## Repo Map

- `coop/`: core runtime implementation
- `tests/`: functional and regression tests
- `benchmarks/`: performance comparisons and tuning harnesses
- `examples/`: runnable sample programs

## Change Safety

- Prefer small, local diffs over broad refactors.
- Do not silently change scheduler or IO semantics without tests.
- Keep docs aligned when behavior or conventions change.
- Preserve hot-path constraints from `DESIGN_IDIOMS.md`.

## Validation Expectations

- Run targeted tests for touched behavior.
- Run full `coop_tests` for scheduler, shutdown, IO, context lifecycle, or perf counter changes.
- Include exact validation commands in handoff notes.

## IO and Sandbox Note

Some environments block or partially restrict `io_uring`. If you hit `uring init failed`, that is an environment problem unless your change touched initialization logic. Validate on a host that permits `io_uring` for final confirmation.

## Autonomous Work Discipline

Rules for long-running or unattended agent sessions (overnight loops, multi-hour campaigns).
They exist because the failure mode of autonomous work is not wrong results — it is legible
activity that closes no questions.

- **Measure before you memo.** If a decision hinges on a quantity that can be measured on this
  machine in less time than the analysis around it would take to write, run the measurement.
  An estimate ("likely", "~13%", "bracketed") standing in for a runnable experiment is a
  process failure regardless of how reasonable the estimate is. If the measurement genuinely
  cannot be run here, record it explicitly as `BLOCKED-ON-MEASUREMENT (cost: ~N min, needs: X)`
  so the gap stays visible.
- **Keep a decision-unknowns ledger, drained cheapest-first.** Maintain the explicit list of
  "what un-run thing would change a call." Drain cheap decision-unblockers before starting new
  exploration.
- **Safety is structural, not genre-based.** A throwaway branch + the full test suite + no
  merge or push is a safe experiment, whether it is a build flag, an assembly variant, or a
  benchmark. Do not classify work as "needs a human" because it feels weighty while running
  read-only audits that feel safe — that inverts the value ordering. Humans gate merges,
  pushes, public API changes, and irreversible actions; branches are for everything else.
- **Delegate; the orchestrating context is for decisions.** Multi-file audits, experiment
  runs, and research go to subagents with fresh context. Burning the coordinating context on
  inline reading ends with compaction destroying exactly the detail being accumulated.
- **Every loop iteration ends with a typed delta**: code, measurement, commit, or —
  explicitly — prose-only. Two consecutive prose-only iterations mean the readable surface is
  exhausted: switch to the measurable/buildable backlog, or stop and say so.
- **Fill wall-clock, not just turns.** End an iteration by launching the long-running job
  (benchmark sweep, build matrix, fuzz run) that the next iteration will harvest.
- **Write findings once.** A result goes to one durable place (issue, ledger, doc) with
  pointers elsewhere. Triple-writing the same finding into a scratch log, a summary, and a
  chat recap is activity, not progress.
