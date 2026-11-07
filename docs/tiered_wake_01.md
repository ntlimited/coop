# Tiered targeted wake protocol

Status: experimental, disabled by default.

A targeted submission needs prompt pickup without keeping an idle CPU busy forever.
`Cooperator::Submit` already supplies the queue and admission contract; this design
changes only its notification and the scheduler's final idle boundary. It also
serves notifications for existing `Cooperate` replies and `Shutdown`. It does not
implement a completion latch, move contexts, or change work-sharing policy.
`InstallShutdownHandler` retains its separate process-wide, async-signal-safe
eventfd bridge to its watcher thread; that bridge calls `ShutdownAll`, whose
individual cooperator notifications use this protocol.

## Covenants

- No admitted notification may be lost between checking work and sleeping.
- No producer may manipulate a target's scheduler, coordinators, or native SQ ring.
- No per-cooperator notification eventfd is created or used when MSG_RING is selected.
  Kernels without it use the existing eventfd transport for COLD notifications.
- No hardware monitor instruction executes without its advertised CPUID feature.
- No unbounded user-mode waiting; pending IO, cancellation and shutdown receive
  service after the configured idle budget. OS preemption is outside that bound.
- No default behavior change, new user-space allocation in the notification backend,
  or parallel submission queue. Existing submission entries and kernel IO request
  resources retain their own allocation contracts. The legacy eventfd drainer
  remains authoritative when the option is off.

## Prior art and scope

This is a variant of the established spin-then-park pattern: the intermediate
state uses address monitoring rather than continuous polling. Intel's
[User Wait Instructions guide](https://cdrdv2-public.intel.com/751859/Power_Mgmt_User_Wait_Instructions_Power_Saving_TechGuide_751859v4.pdf)
describes UMONITOR/UMWAIT and the OS-controlled maximum wait; AMD's
[Architecture Programmer's Manual, Volume 3](https://docs.amd.com/v/u/en-US/24594_3.37)
describes MONITORX/MWAITX, including spurious returns and the timer enable bit.
Neither instruction is a synchronization primitive: the atomic protocol below
provides ordering. The monitored address is armed before rechecking its value.

The COLD transport uses Linux
[IORING_OP_MSG_RING](https://kernel.googlesource.com/pub/scm/linux/kernel/git/axboe/liburing/+/refs/heads/master/man/io_uring_prep_msg_ring.3).
Opt-in construction probes MSG_RING support once before accepting submissions.
If the sender cannot be prepared or the probe cannot establish support, construction
releases the sender and selects the existing eventfd transport and submission drainer
for COLD notifications. HOT and SEMI-COLD retain their store/monitor behavior.
`WakeUsesMsgRing()` and the benchmark telemetry field `cold_transport` report the
selection. A supported sender is owned from construction through teardown, including
cooperators that are never launched.
A private, mutex-serialized, non-SINGLE_ISSUER sender ring permits ordinary external
threads to notify, without entering the target's SINGLE_ISSUER ring. Waiting for
the sender CQE establishes successful delivery; failures are fatal rather than
silently turning an accepted notification into a hang. This deliberately conservative
prototype pays sender serialization and a sender completion; future optimization
must preserve failure handling and destination lifetime. A COLD send blocks its
producer OS thread until kernel acknowledgement, so producer scheduling/fairness
under that wait is part of adoption evidence, not a bounded-latency promise.

Locally, `cross_thread_substrate_01.md` remains the work-sharing contract. Its
measured parked Submit cost motivates this independent experiment; its opt-in
stealer, deque and local-shed Coordinator remain untouched. `continuations_01.md`
continues to require registration, firing and cancellation on the owner thread.
This protocol is notification below those APIs, not cross-thread Coordinator release.

## Protocol and proof

Each cooperator owns an isolated cache-line atomic `Doorbell`. Its low two bits
are HOT, SEMI-COLD or COLD; upper bits count notifications modulo 2^62. The word
uses sequentially consistent operations. Queue publication remains under the
existing submission mutex and release/acquire submission flag.

A producer first publishes its work or shutdown condition, then increments the
word by four. The returned old state is its decision: HOT or SEMI-COLD needs no
syscall; COLD requires the selected kernel notification: MSG_RING with null user data, or
a write to the existing eventfd. The drainer keeps an eventfd read in flight, so
a write before kernel entry remains observable. The native
completion dispatcher ignores that reserved null cookie. The message is a
notification, with no Handle or PendingOps ownership.

The owner snapshots the doorbell **before** checking submission/shutdown and
polling IO. HOT briefly polls. SEMI-COLD is entered with compare-and-exchange,
then repeatedly arms the hardware monitor, rechecks the entire word, and waits
for a bounded timer quantum. It checks work/IO between quanta. Spurious hardware
returns are legal. Resetting HOT preserves the notification generation.

At escalation the owner snapshots the word, checks readiness, then CASes exactly
that value to COLD. If a producer's increment precedes this CAS, the CAS fails
and sleeping is abandoned. If the CAS precedes the increment, the producer sees
COLD and sends the selected kernel notification. A delivered CQE or eventfd count
persists even if the owner has not yet entered `WaitAndPoll`. There is no interval in which the owner can sleep after
a producer chose a store-only notification against an earlier generation.
The queue predicate also covers a producer paused between queue publication and
notification. An unrelated IO completion can wake the owner first; a delayed
message is harmless. The owner returns HOT before resuming application work. Once shutdown kill has
been handled, the shutdown flag stops being a runnable-work predicate: delayed
cleanup IO must still be allowed to park in COLD until its CQE arrives. Conversely,
when shutdown has no blocked work or pending IO left, the idle boundary must return
to the loop-exit condition rather than entering another COLD wait.
Generation wrap requires 2^62 notifications between one snapshot and CAS; the
protocol assumes that cannot happen within one bounded idle attempt.

## Lifetime, shutdown and cancellation

Bind a prepared MSG_RING sender after the native ring on the owning Launch thread.
For eventfd, launch the existing kill-aware submission drainer before entering the loop.
Publish COLD only after initialization. Notifications before Launch remain in the
queue; HOT stores suffice. Sender initialization and teardown share its mutex
with sends. Teardown clears the destination under that mutex before destroying
the native ring, so a delayed producer cannot send to a recycled descriptor.
The Cooperator object must outlive all callers, as with the existing Submit API.
No new acceptance gate is introduced: `PushSubmission` retains shutdown rejection
and completion-notification exceptions. The owner drains admitted replies and
normal cancel CQEs before ring destruction. The eventfd submission drainer is
excluded from the shutdown kill sweep while admitted Cooperate replies remain
outstanding. An owner-thread count increases on accepted dispatch and decreases
at the shared reply-invocation boundary. Shutdown retains the drainer for admitted
replies, runnable cleanup, continuations and counted native IO. Retirement is rechecked
on the owner thread after idle polling and continuation draining, including the polls
inside tiered waiting, before entering a kernel wait. If only the drainer's own read
remains, it is cancelled rather than parked on indefinitely. Queued timers stay
passive during shutdown; metadata owners cancel their registrations after join.
A timer registration alone does not extend cooperator lifetime. This rule applies
to every COLD transport and to the legacy backend. Kill delivery uses the existing
owner-thread submission path; the wait protocol never performs a cross-thread kill.

## Bounds and configuration

`CooperatorConfiguration::tieredWake` defaults false. `wakeHotUs` and
`wakeSemiColdUs` are resource budgets, not correctness knobs; each is clamped to
100 microseconds. Initial experimental budgets are 1 and 10 microseconds,
respectively. Zero skips that tier. Their adoption values require measurement;
these are exploration starting points, not calibrated defaults.

The startup CPUID probe selects WAITPKG, then MONITORX, otherwise PAUSE (aarch64:
YIELD). Each hardware wait requests at most 1000 counter ticks and every outer
iteration checks monotonic time. Intel UMWAIT requests C0.2 (control operand zero); the kernel may restrict it to
C0.1. The kernel's
IA32_UMWAIT_CONTROL limit may return earlier and is never changed or bypassed.
AMD MWAITX always enables its bounded timer. The intermediate tier delays native
IO service by at most the configured budget plus one quantum and preemption;
readiness checks poll IO throughout. Every poll services expired userspace timers
and restores the nearest-deadline kernel timer before another monitor or kernel wait.
A cancelled earliest sleep may leave an obsolete timeout; consuming that timeout
must rearm the next queued sleep even when no context becomes runnable. Virtual
time never arms a kernel timer. After escalation the CPU blocks indefinitely
in the kernel until real work. No periodic cold timer or unbounded idle spin.
RAPL/package power and SMT interference remain adoption evidence, not claims
inferred from instruction counts or CPU time.

## Implementation and validation slices

1. `detail/doorbell.{h,cpp}`, `io/wake_ring.{h,cpp}`, `cooperator.{h,cpp}`,
   `cooperator_configuration.h` and the null CQE cookie: one opt-in prototype
   retaining queue/admission semantics. First regression: constructing with the
   option must create no eventfd when MSG_RING is available (fails on the legacy
   implementation); a forced-unsupported probe must construct successfully and
   deliver repeated COLD submissions, cancellation and shutdown through eventfd.
2. `tests/test_wake.cpp`: deterministic both-orderings of the cold CAS race,
   generation preservation, monitor recheck, runtime bursts, prelaunch submission,
   cross-thread kill and shutdown with in-flight cancellable IO. Existing shutdown,
   Cooperate, IO and lifecycle filters remain applicable.
3. `benchmarks/bench_wake.cpp`: legacy versus HOT/SEMI-COLD/COLD, warm and idle-state synchronized,
   1 through 16 cooperators; record pickup latency percentiles (also medians grouped by actual notification tier), end-to-end batch
   throughput, observed tier and selected ISA. The opt-in `wakeDiagnostics` counters add one atomic increment per notification;
   all prototype measurements must disclose that instrumentation. Record CPU/system time, faults and
   voluntary/involuntary switches over latency preparation plus throughput (not
   a per-notification or idle-power estimate). A tier result is inconclusive if not observed;
   unsupported ISA is not a measurement of that ISA. Same executable selects modes.

The prototype is committable without enabling the option for any consumer. A
later adoption slice needs measured per-host budgets, full shutdown/race review,
idle power/fairness evidence and application parallelism panels. A latch must
publish its completion through the existing owner-thread notification boundary;
it must not grow another eventfd or reach into the doorbell. Existing Submit users
need only configure their owning cooperators, after adoption evidence warrants it.


## Prototype measurements and adoption limit

An optimized `-march=znver4` run on a Zen 5 virtual machine used eight allowed
CPUs, 256 KiB default context stacks, and notification diagnostics enabled.
Three rounds used 1,000 latency samples per row and 32,768 batch submissions.
The same executable selected the legacy eventfd control or the experimental
protocol. The CPUID probe selected PAUSE: this environment did **not** expose
MONITORX/MWAITX. These are measurements of the portable intermediate tier,
not hardware-monitor results.

For one cooperator, medians of the three per-round pickup medians were:

| notification regime | pickup median | batch throughput |
| --- | ---: | ---: |
| eventfd, parked | 9.16 us | 6.27 M/s |
| HOT | 0.17 us | 9.29 M/s |
| SEMI-COLD, PAUSE fallback | 0.19 us | 9.48 M/s |
| COLD, MSG_RING | 8.23 us | 9.44 M/s |

Legacy warm pickup was 0.34 us; experimental warm pickup was 0.18–0.19 us.
The legacy parked control waits 50 us before sampling. Experimental COLD samples
synchronize on the published COLD state, which includes the race-safe interval
before kernel entry; they do not witness that the OS thread is already off CPU.
Thus the COLD comparison does not isolate an equal-idle-depth OS wake floor.
Matched parking/off-CPU evidence is required before making that stronger claim.
SEMI-COLD figures use the actual notification tier, not the requested state;
short monitor quanta can return to HOT before a producer reaches notification.
At eight and sixteen cooperators, COLD batch throughput was 0.238 and 0.236 M/s,
against eventfd's 0.255 and 0.260 M/s. Sixteen cooperators oversubscribed the
eight allowed CPUs. Faster pickup does not establish better sustained throughput. Batch throughput
measures the selected configuration end to end; backlog can keep its notifications
in HOT even when the idle budget selects COLD. It is not a claim that every batch
notification used the named tier. Reported tier sample counts cover latency probes.
A separate syscall-summary diagnostic (100 samples plus both throughput phases,
one cooperator) counted 131,668 calls for eventfd, 446 for the intermediate-tier
configuration and 51,416 for COLD; these totals include preparation and teardown,
not a claimed syscall cost for an individual notification.

Do not enable this protocol by default from these results. Hardware-monitor
latency, budget/power calibration, idle package power, SMT interference and
construction/retirement cost remain adoption requirements. The initial 1/10 us
resource budgets remain experimental; the forced-tier panel uses 100 us windows
to collect observations, and does not tune those defaults.


Ivy Bridge local `cycles:u` profiles of the same one-cooperator workload used
10,000 latency samples per regime and frame-pointer sampling. The PAUSE helper
accounted for 60.3% of the legacy profile and 19.9% of the intermediate-tier
profile. Producer waiting dominates the legacy profile; the owner's
`WaitTiered` accounts for 8.45% of owner-thread cycles in the intermediate run.
These whole-panel diagnostics include setup and producer work and do not isolate
the notification body or establish package-power savings.


A second optimized `-march=graniterapids` panel used four allowed CPUs and the
same stack, diagnostics, sample counts and three-round protocol. CPUID selected
WAITPKG and the panel completed, including actual SEMI-COLD notifications.
For one cooperator, actual-tier pickup medians were HOT 0.516 us, SEMI-COLD
0.910 us and COLD 2.614 us, versus the legacy parked control's 11.769 us.
The SEMI-COLD figure is the median of the three actual-tier medians
(0.247, 0.966 and 0.910 us), not the mixed-tier configuration median of 0.444 us.
Experimental warm pickup was 0.172–0.173 us versus legacy's 0.425 us.
Idle-configuration batch throughput was 3.88/3.96/3.94 M/s for HOT/SEMI-COLD/COLD,
versus eventfd's 1.85 M/s. With sixteen cooperators oversubscribing four CPUs,
warm pickup was approximately 20 ms across all modes: avoiding notification
syscalls does not supply missing execution capacity. The timing/parking and
power qualifications above still apply. This validates execution of the WAITPKG
prototype on that environment, not MONITORX or default-on adoption.
