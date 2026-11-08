# Cross-thread countdown completion

`CountdownLatch` joins a fixed set of predecessors on one cooperative waiter. It
owns its counter inline; the active wait frame hosts its signal and intrusive notification. Arrivals may
come from other cooperators or ordinary threads. It does not own their work.

## Covenants

- No per-instance kernel object or allocation. The owner cooperator's existing
  submission queue and configured wake transport carry the terminal notification.
- No foreign thread touches a live Context, Signal or Coordinator. Arrive only
  changes the atomic count, enqueues the frame-hosted node and rings the doorbell.
  The inline submission notifies the signal on the owner's thread without a
  context switch. The submission consumer finishes with the node before the
  waiter can resume and destroy it. Completion enqueue and wake notification share
  the queue lock, so consumption cannot outrun the producer's last owner access.
- No lost registration race. Wait publishes its owner and node before setting
  the waiter bit with an acquire/release RMW on the same word as the countdown.
  A last arrival either observes that bit and queues exactly once, or publishes
  zero for the later waiter to acquire. Concurrent arrivals form an RMW chain
  publishing every predecessor's preceding writes.
- No shutdown refusal. Completion nodes bypass public work admission. A
  registered wait keeps the scheduler alive and retains the shared wake reader.
  The wake reader is excluded from the shutdown kill sweep and retires only
  when runnable work, continuations, counted IO and registered waits drain.
  Retirement is rechecked after idle polling and before kernel parking.
  MSG_RING needs no reader; the same wait count retains its scheduler.
  Terminal notifications use the normal tiered wake when enabled, including
  the eventfd fallback. Doorbell writes retry EINTR; EAGAIN is already readable.
- Cancellation cannot abandon accepted predecessors. WaitKill invokes the
  cancellation callback promptly, then joins the same terminal notification.
  A kill delivered during deadline-timer cleanup is rechecked before success.
  Coordination errors remain distinct from kills; the same callback cancels
  dependent work before draining. The callback must return normally; arrivals
  must eventually occur, including
  refused work's accounted arrivals. A missing arrival is a caller deadlock.

The caller owns storage and lifetime: publish only a prepared latch, arrive
exactly the prepared number of times, wait once per round, and join all users
before reset or destruction. An unused prepared latch may be destroyed if it
has never been published. IsReady observes the countdown, not retirement of a
registered wait; it is not a replacement for joining that wait.

WaitKill's optional registration observer runs after publishing a pending waiter
and before suspension. It is for diagnostics and deterministic scheduling tests,
may yield, and must return normally. It does not replace the cancellation callback.
