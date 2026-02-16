# Tests

## Framework
- GTest with `gtest_discover_tests`. Test binary: `build/{debug,release}/bin/coop_tests`
- `RunInCooperator` (`test_helpers.h`) wraps test bodies: `Submit()`s a lambda into a
  cooperator, the lambda calls `Shutdown()` when done, `Thread` destructor joins on exit.
  GTest assertions work inside because the join establishes happens-before.
- Use `--gtest_filter='Pattern*'` for targeted runs during development. Run the full suite
  when making foundational changes.

## Build order
- Always run debug tests first. Guard pages (`mmap`/`PROT_NONE`) are active in debug builds
  and turn use-after-free into immediate SIGSEGV at the exact point of corruption. Release
  tests use `malloc`/`free` where the same bugs silently corrupt.
- Run with GDB attached (`gdb --args bin/coop_tests --gtest_filter=...`) when diagnosing
  hangs or crashes — a hanging test under GDB can be interrupted to get a backtrace.

## Pitfalls

### Yielded-list ordering
`Spawn` pushes the calling context to the yielded list before switching to the new child.
If a test spawns a parent which then spawns children that block or yield, the cooperator
may resume the outer context before the parent finishes spawning. This causes the outer
context to act on an incomplete tree.

Fix: use a `Coordinator` to synchronize. The parent `TryAcquire`s it at the start, releases
it after all children are spawned; the outer context `Acquire`s it to block until ready.
```cpp
coop::Coordinator ready;

ctx->GetCooperator()->Spawn([&](coop::Context* parent)
{
    ready.TryAcquire(parent);
    // ... spawn children ...
    ready.Release(parent, false);
    parent->GetKilledSignal()->Wait(parent);
}, &parentHandle);

ready.Acquire(ctx);
ready.Release(ctx, false);
// parent is now fully set up
```

### Stack budget
Default context stack is 16KB (`s_defaultConfiguration.stackSize`). Operations running on a
context stack — including framework internals invoked from that context — must fit within this.
Use `SpawnConfiguration` with a larger `stackSize` when a context needs more, or prefer
iterative algorithms over recursive ones for depth-variable work.
