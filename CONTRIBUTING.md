# Contributing to coop

## Workflow

1. Build debug first.
2. Run targeted tests for the code you changed.
3. Run full `coop_tests` before merge when touching core runtime or IO.
4. Update docs when semantics or conventions change.

## Build and Test

Debug:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
./build/debug/bin/coop_tests
```

Release:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
./build/release/bin/coop_tests
```

Targeted test example:

```bash
./build/debug/bin/coop_tests --gtest_filter='ShutdownTest.*'
```

## Source of Truth for Architecture

- Start with `CLAUDE.md`.
- For runtime internals, read `coop/CLAUDE.md`.
- For component-specific behavior, use component `CLAUDE.md` files.
- Use `DESIGN_IDIOMS.md` for API/layout decisions.

## Code Expectations

- Favor stack allocation and RAII where practical.
- Keep hot paths contiguous and predictable.
- Avoid virtual dispatch in per-element hot loops.
- Use typed wrappers for semantically distinct integer domains.
- Keep comments focused on intent and invariants.

## Documentation Expectations

Update docs in the same change when you:
- alter scheduler behavior
- alter context lifecycle semantics
- alter IO/uring initialization or fallback behavior
- add/remove public API or configuration fields

## Notes on io_uring Environments

`coop` depends heavily on `io_uring`. CI or sandbox environments may block required syscalls or flags. If tests fail at uring initialization, verify whether the environment supports `io_uring` before treating it as a regression.
