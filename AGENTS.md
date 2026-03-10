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
