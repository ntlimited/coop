# Benchmarks

## Framework
- Google Benchmark. Binary: `build/{debug,release}/bin/coop_benchmarks`
- Always run benchmarks in **release mode**. Debug builds have guard pages and asserts that
  distort timing.
- Use `--benchmark_filter=Pattern` for targeted runs.
- Use `--benchmark_repetitions=N` (default: 3 in capture.sh) for statistical confidence.

## Capture and Compare

### Running Benchmarks
Use `capture.sh` to run benchmarks with full environment context:
```bash
# Full suite
./benchmarks/capture.sh

# With label for identification
```

Output goes to `benchmarks/reports/runs/` (gitignored). Each run produces:
- `.json` — machine-readable Google Benchmark output
- `.md` — human-readable report with environment table + results

### Comparing Runs
```bash
./benchmarks/compare.sh reports/runs/baseline.json reports/runs/candidate.json

# With filter
./benchmarks/compare.sh base.json cand.json --filter='Insert'
```

Color-coded output: green = improvement (>2%), red = regression (>2%).

### Investigation Reports
For meaningful before/after results, create a numbered report in `benchmarks/reports/`.
See `benchmarks/reports/CLAUDE.md` for the template and workflow.

## Benchmark Categories

Benchmarks are named `BM_{Area}_{Operation}[_{Variant}]`. The area prefix determines
which filter to use when a component changes.
### Scheduler / Cooperator
Filter: `--filter='BM_Scheduler_|BM_Coop_|BM_Pthread_|BM_AcquireRelease|BM_Coordinator'`

| Benchmark | Measures |
|-----------|----------|
| `BM_Scheduler_Yield` | Single-context yield round-trip |
| `BM_Scheduler_Yield_Scaled` | Yield cost at 1–64 contexts |
| `BM_Scheduler_SpawnYieldExit` | Full context lifecycle |
| `BM_AcquireRelease` | Uncontended coordinator fast path |
| `BM_AcquireRelease_Contended` | Contended coordinator |

### IO
Filter: `--filter='BM_IO_'`

| Benchmark | Measures |
|-----------|----------|
| `BM_IO_Accept` / `BM_IO_Connect` | Connection establishment |
| `BM_IO_Recv` / `BM_IO_Send` | Message throughput (async + blocking) |
| `BM_IO_Read` / `BM_IO_ReadFile` | File I/O |
### HTTP
Filter: `--filter='BM_HTTP_'`

## Coding Standards

### Naming Convention
```
BM_{Area}_{Operation}[_{Variant}]
```
- **Area** matches the component (IO, HTTP, Scheduler, etc.)
- **Operation** is the action measured (Insert, Find, Scan, Yield, etc.)
- **Variant** distinguishes sub-cases (Full, Range, DeadRows, etc.)

Comparative benchmarks use matching suffixes (e.g., `BM_Coop_Yield` vs `BM_Pthread_Yield`).

### Style
- Normal project style applies
- Each benchmark function should have a comment block explaining what is measured and why
- Group related benchmarks together in the source file with section separators (`// ---`)
- Helper functions shared across benchmarks in the same file go at the top

### Scale and Methodology
- Test at multiple N values to expose O(log N) / O(N) behavior and cache hierarchy effects
- Use `RangeMultiplier(10)->Range(lo, hi)` for log-scale sweeps
- Use `SetItemsProcessed()` so throughput (items/s) is reported
- Use `UseRealTime()` for anything involving io_uring or cross-thread coordination
- Use deterministic seeds (e.g., `0x5eed...`) for reproducibility
- Exclude setup/teardown with `PauseTiming()` / `ResumeTiming()` where the setup cost is
  substantial and not part of what's being measured
- Use `DoNotOptimize()` to prevent dead-code elimination of computed results

### Adding New Benchmarks
1. Choose the correct file based on area. IO → `bench_io.cpp`, etc.
2. Follow the naming convention. Ensure `--benchmark_filter` can isolate it.
3. Add the benchmark to the category table in this CLAUDE.md.
4. Run `capture.sh` with the new benchmark filtered to establish a baseline.
5. If the benchmark requires new helper functions, keep them file-local (static).

### Regression Testing After Code Changes
When modifying hot-path code:
1. Identify which benchmark categories are affected (use the tables above).
2. Run `capture.sh` with the appropriate filter before and after the change.
3. Use `compare.sh` to check for regressions.
4. Acceptable variance: CV% < 3% for timing benchmarks. If CV is higher, increase `--reps`.
5. Flag any regression >5% in time or >5% drop in throughput.
