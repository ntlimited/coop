# Deployment & Host Tuning

Host-level configuration that moves coop's performance more than most feature work —
several items below are individually worth double-digit percentages, and none require a
line of coop code. Sources: docs/zero_copy_survey_2026-08.md and the research behind
docs/improvement_sequence_01.md; numbers marked with their origin.

## Disk tier (the cache path)

- **Filesystem: XFS, 4K blocks.** XFS advertises async buffered writes to io_uring
  (`FOP_BUFFER_WASYNC`); ext4 does not — on ext4 every buffered write through the ring
  punts to an io-wq worker AND serializes per inode (io-wq hashes buffered writes by
  inode). A 1K-block filesystem forces read-modify-write on 4K-aligned IO (ScyllaDB
  measured most of 14.5 GB/s lost to exactly this).
- **Block scheduler: `none` for NVMe.**
  `echo none > /sys/block/<dev>/queue/scheduler`. CHEOPS'23 measured schedulers costing
  14-57% for io_uring workloads — mq-deadline's global lock consumed ~80% of CPU cycles
  at 16 threads.
- **Know the punt cliffs.** `io::ProbeBlockLimits(path)` logs `max_hw_sectors_kb`,
  `nr_requests`, and `max_segments` for the device backing a path. A single transfer
  larger than max_hw_sectors_kb, more in-flight requests than nr_requests, or a
  scatter-gather list past max_segments silently punts to io-wq (no error — just a
  worker thread and interrupt-era latency). Cap per-op disk transfers at
  min(max_hw_sectors_kb, 512KiB) and bound in-flight O_DIRECT batches below nr_requests.
- **Watch `io::IowqWorkerCount()`.** Zero is the healthy steady state for a
  socket-driven cooperator. Persistently nonzero means something is punting.

## Network

- **Socket buffers and qdisc**: default `net.core.{r,w}mem_max` caps autotuning well
  below what 10G+ streaming wants; raise to 16MB+. `fq` qdisc for pacing-heavy egress.
  IRQ affinity: keep NIC queue IRQs off the cooperator cores (worth >25% on high-rate
  shuffles in the surveyed literature; on chiplet CPUs keep IRQs and their consumers on
  the same die).
- **SO_REUSEPORT sharding** (RunServer): one listener per cooperator is the declared
  topology — kernel-side connection distribution, no shared accept fd.

## Limits

- **RLIMIT_NOFILE**: a proxy holds 2-6 fds per in-flight request (downstream socket,
  upstream socket, cache file, pipes). Size deliberately at startup; do not rely on
  library calls to grow it (coop never mutates process rlimits — beware that liburing's
  `io_uring_register_files_sparse` DOES setrlimit as a side effect; coop avoids it).
- **RLIMIT_MEMLOCK**: registered buffers account against it (pre-5.12 kernels strictly;
  newer kernels account differently but lockups still surface as -ENOMEM at
  registration). If registration fails at startup, this is the first thing to check.
- **Pipe accounting**: an unprivileged UID gets 1024 pipes at 64KiB; beyond
  `fs.pipe-user-pages-soft` every new pipe is silently created at 8KiB — an 8x
  throughput cliff on splice paths with no error anywhere. coop's PipePool caps at 8
  pipes per cooperator; if you build something pipe-hungry, raise the sysctl or budget
  under it. The accounting is per-UID across all processes.

## io_uring feature floors (this codebase, liburing 2.6)

| Feature | Kernel | Notes |
|---|---|---|
| Ring-fd registration | 5.18 | On by default (warn-and-degrade below) |
| Fixed-file table (`io::registered`) | 5.1+ | Opt-in per descriptor |
| COOP_TASKRUN | 5.19 | Default on |
| DEFER_TASKRUN | 6.1 | Off — measured 20-30% slower for coop's submit-then-wait shape |
| Provided buffer rings (pbuf) | 5.19 | Opt-in via `bufferRingEntries` |
| Multishot accept | 5.19 | Track A (improvement_sequence_01) |
| SEND_ZC | 6.0 | Track B; useless <~1KiB, silently copies on loopback |
| io-wq worker caps | 5.15 | Opt-in via `iowqMax*Workers` |
| Registered-buffer recv | — | Exists in NO released kernel; do not plan for it |
| RWF_DONTCACHE | 6.14 | Future: per-IO page-cache-drop for large fills |
| clone_buffers | 6.12 (+liburing 2.8) | Future: cheap cross-ring arena sharing |
| FIXED_FD_INSTALL | 6.8 | Future: escape hatch direct fd -> real fd |
| NAPI busy poll | 6.9 | Future |
| zcrx | 6.15 | Future; requires DEFER_TASKRUN (see above) |

Capability probes succeed on vulnerable kernels — the floor table is a security
statement as much as a feature one. Registration machinery is the most-exploited
io_uring surface (CVE-2023-2598, CVE-2022-29582, CVE-2024-0582); every registration
feature in coop is opt-in and off by default so an unconfigured deployment does not
touch that surface.

## SQPOLL: a core-donation trade, never a default

SQPOLL donates one kernel thread per ring (coop: per cooperator). It measured +44-70%
for coop under saturating concurrent load — and it measures NEGATIVE when the donated
cores could have run cooperators instead, when load is bursty (waking an idle SQPOLL
thread costs ~30us), or when user contexts compute meaningfully between polls (a busy
cooperator starves the poller's benefit; the published IOPS-collapse critiques of
inline work on polling rings are measurements of exactly this). Enable it only for
saturated, IO-dominant deployments with cores to spare, and tune `sqpollIdleMs` to the
burst gap. A pinned SQPOLL thread whose CPU is offlined silently stalls all submission.

## Verifying a deployment

1. `io::ProbeBlockLimits(cacheDir)` at startup — logs the cliff parameters.
2. `io::IowqWorkerCount()` on the status page — nonzero steady state = something punts.
3. Confirm `uring init flags` log line shows the expected setup flags.
4. Watch for `register_*` warn lines — every registration degrade is logged, never
   silent.
5. The load average matters more than any of this: coop's timer covenants (and any
   latency SLO) presuppose cooperator threads actually get scheduled. A saturated host
   fails timing assertions before it fails throughput.
