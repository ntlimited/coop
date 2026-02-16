# Benchmarks

## Framework
- Google Benchmark. Binary: `build/{debug,release}/bin/coop_benchmarks`
- Always run benchmarks in release. Debug builds have guard pages and asserts that distort
  timing. Use `--benchmark_filter=Pattern` for targeted runs.

## Coding Standards

### Style
- Normal project style applies

## Best Practices
- Test functionality at various scales and accounting for synthetic behaviors — cache line
  alignment most commonly
- Benchmarks should be named in a manner that makes filtering for them easy in the compiled
  set across all files
- Comparative benchmarks should be named so that they can easily be matched against each other
  (e.g. `BM_Coop_Yield` vs `BM_Pthread_Yield`)
- A working directory should be created and never committed, used to track benchmark results
  over time. Files should contain the results from the run as well as relevant metadata (load
  averages, etc) and be named in a manner that includes their run timestamp and git revision.
