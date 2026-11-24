# 001: Directional hot-loopback note on HTTP file-response sends

The signal-safe file path had lower median wall and process CPU time than the
reconstructed 4 KiB copy model for all three transports in two interleaved runs. The primary
run followed native build and test completion. A large scheduling burst affected the first
run, retained in the appendix. Shared-host variance still prevents a stable throughput or
capacity claim.

## Method

`bench_http_file` alternates two paths in one Release executable, reversing their order on
each repetition. Each path sends a valid HTTP/1.1 200 response with Content-Length and a
32 MiB body. The receiver parses the header boundary, verifies the declared length, and
counts the complete body. Socket/handshake/file setup is outside the timed region. Headers,
body transfer, and receiver completion are timed for both paths. TCP_NODELAY matches the
HTTP server's accepted-socket policy.

- Before model: one cooperative 4 KiB `ReadKill`, followed by the native transport's timed
  partial `Send`, repeated across the file. This reproduces the superseded server file-send
  algorithm in the same executable; it is not the previous commit's HTTP server or a
  separately compiled historical binary. It bypasses `Connection::Sendfile` and its
  2 MiB fairness yield, which the after path pays.
- After: `http::Connection<Transport>::Sendfile`. Plaintext and kTLS use forced-async
  io_uring file-to-pipe splice followed by a nonblocking pipe-to-socket splice. The latter
  temporarily blocks SIGPIPE without holding a signal mask across a cooperative wait.
  kTLS stages at most 16383 bytes, uses SPLICE_F_MORE, and flushes records explicitly with
  MSG_NOSIGNAL so later kernel retries during close retain safe signal behavior. Memory-BIO
  TLS uses a cooperative 16 KiB file read followed by a complete TLS record write. Its
  comparison mixes the larger TLS record size with the copy-path change; memory-BIO
  results do not measure zero-copy. Plaintext and kTLS are the zero-copy comparisons.
- Source: a resident 32 MiB memfd populated before timing. Network: TCP loopback, one sender
  and receiver on one cooperator. This does not measure cold-page stalls or validate
  the cold-page motivation for IOSQE_ASYNC, nor concurrent connections.
- Two runs of seven repetitions per path and transport. The primary run followed Debug
  and ASan validation; three Bazel compile jobs remained active. Wall time uses
  MonotonicMicros; CPU time is process-wide user plus system CPU from
  `getrusage(RUSAGE_SELF)`. Process CPU tracked wall time in this run; hidden io-wq
  worker time does not explain the observed gap. This is not a separate accounting of
  kernel worker CPU.
- kTLS was active, with `ktls_tx=1 ktls_rx=1` in the measured run. Both peers used TLS 1.2
  and AES-128-GCM. The kernel module was enabled with `sudo -n modprobe tls`; the same native
  handshake probe then changed from inactive flags to active TX/RX.

## Environment

| Item | Value |
| --- | --- |
| Date | 2026-11-24 |
| Kernel | Linux 6.8.0-142-generic, x86_64 |
| CPU | Intel Xeon E5-2697 v2 at 2.70 GHz; 4 visible CPUs |
| Compiler | GCC 13.3.0, Ubuntu 24.04 |
| Build | Release, -O3, LTO; COOP_PERF_MODE=2 |
| OpenSSL | 3.0.13 |
| liburing | 2.5 |
| spdlog | 1.16.0 from a local source override |
| CPU placement | Unpinned shared host; frequency governor unavailable |
| Host load | Primary run: 1-minute load 7.15 before and 6.38 after |

## Directional observations

All times below are medians for one hot 32 MiB loopback response. These observations
are directional, not stable speedup or capacity estimates. CPU values are RUSAGE_SELF
process measurements. Seven pairs and shared-host load do not establish a stable ratio.

| Transport | Before wall ms | After wall ms | Before CPU ms | After CPU ms | Wall CV before / after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Plaintext | 576.191 | 205.556 | 570.406 | 202.795 | 6.4% / 12.3% |
| Memory-BIO TLS | 951.913 | 488.521 | 941.222 | 463.592 | 9.5% / 9.5% |
| kTLS | 777.897 | 390.953 | 776.697 | 390.543 | 4.0% / 8.9% |

The after path won all seven paired wall-time comparisons for each transport. The
benchmark guide's 3% timing-CV target remained unmet after the quieter repeat: CV ranges
from 4.0% to 12.3% and uses sample standard deviation divided by the mean. Both complete
runs are retained; no outliers were removed. A dedicated, quiet host is needed for precise
regression thresholds.

## Exact commands

The final library and integrated benchmark target were built in an isolated worktree
with cached dependency overrides:

```sh
cd /tmp/coop-review-sigpipe
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCOOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK=ON \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG=/tmp/coop-build-deps/spdlog \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/tmp/coop-build-deps/googletest \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLEBENCHMARK=/tmp/coop-build-deps/googlebenchmark
cmake --build build/release --target bench_http_file -j2
(cd build/release/bin && \
  stdbuf -oL ./bench_http_file 7 \
  > /tmp/coop-review-sigpipe-http-file-bench-quiet.log 2>&1)
```

The integrated target generates cert.pem and key.pem in its binary directory. The measured
source includes the final record-flush progress accounting; the executable exited zero and
reported `ktls_tx=1 ktls_rx=1` in both runs. The unchanged executable's SHA256 was
`6153d1e8022cb32f64fb9458b4ca21972ccafbcff8ab7f867ee5d1fa5576c4df`.
The earlier identical command wrote `/tmp/coop-review-sigpipe-http-file-bench-final.log`;
the repeat wrote the `-quiet.log` file above.

Bazel also exposes `//:bench_http_file` with `:test_certs` as runtime data. The benchmark
now exits nonzero if either required kTLS direction fails to activate; it no longer
skips that transport with a successful process exit. The recorded historical runs
above verified both activation flags before this exit-status correction.

## Raw paired samples

Times are milliseconds; repetitions reverse execution order on alternate rows.

| Transport | Repetition | Before wall | After wall | Before CPU | After CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| plaintext | 0 | 574.772 | 206.781 | 565.144 | 202.795 |
| plaintext | 1 | 574.096 | 196.408 | 561.730 | 194.244 |
| plaintext | 2 | 616.432 | 164.667 | 606.071 | 161.422 |
| plaintext | 3 | 576.191 | 245.142 | 570.406 | 236.928 |
| plaintext | 4 | 553.652 | 198.137 | 546.227 | 195.937 |
| plaintext | 5 | 645.249 | 205.556 | 639.658 | 203.514 |
| plaintext | 6 | 650.816 | 228.204 | 645.185 | 225.586 |
| memory_tls | 0 | 951.913 | 505.714 | 941.222 | 502.100 |
| memory_tls | 1 | 1131.055 | 588.756 | 1116.619 | 528.803 |
| memory_tls | 2 | 959.207 | 488.737 | 949.152 | 485.256 |
| memory_tls | 3 | 887.379 | 446.552 | 886.710 | 444.569 |
| memory_tls | 4 | 1001.711 | 488.521 | 1000.670 | 446.094 |
| memory_tls | 5 | 879.671 | 465.455 | 876.983 | 463.592 |
| memory_tls | 6 | 878.425 | 462.703 | 844.907 | 462.292 |
| ktls | 0 | 777.897 | 377.906 | 776.697 | 377.772 |
| ktls | 1 | 765.120 | 384.267 | 764.614 | 383.998 |
| ktls | 2 | 771.505 | 390.953 | 770.636 | 390.543 |
| ktls | 3 | 842.217 | 421.582 | 841.450 | 421.092 |
| ktls | 4 | 803.729 | 386.944 | 803.030 | 386.562 |
| ktls | 5 | 825.050 | 407.530 | 811.709 | 407.188 |
| ktls | 6 | 760.977 | 481.792 | 760.011 | 409.740 |

## Appendix: scheduling-burst run, unusable for quantitative comparisons

The first final-code run also completed all 42 responses with exit zero and active kTLS.
Its 1-minute host-load samples were 9.34 before, 22.63 during, and 17.06 after the run.
The first two plaintext pairs coincided with a scheduling burst: wall times ranged from
3.6 to 13.6 seconds, versus below 1.4 seconds in later plaintext pairs. This prompted the
planned repeat after native validation finished. The after path won six of seven plaintext
pairs and all memory-BIO TLS and kTLS pairs.

| Transport | Before wall ms | After wall ms | Before CPU ms | After CPU ms | Wall CV before / after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Plaintext | 944.658 | 293.892 | 659.411 | 204.894 | 149.8% / 142.8% |
| Memory-BIO TLS | 1129.402 | 560.707 | 949.103 | 489.451 | 8.0% / 11.0% |
| kTLS | 971.081 | 526.611 | 836.126 | 418.944 | 9.1% / 14.1% |

### Raw samples during native builds

Times are milliseconds; repetitions reverse execution order on alternate rows.

| Transport | Repetition | Before wall | After wall | Before CPU | After CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| plaintext | 0 | 3601.474 | 4737.244 | 1666.185 | 1594.225 |
| plaintext | 1 | 13573.268 | 5278.704 | 4451.027 | 2165.644 |
| plaintext | 2 | 1384.807 | 291.968 | 670.506 | 196.411 |
| plaintext | 3 | 926.562 | 302.154 | 646.969 | 198.802 |
| plaintext | 4 | 836.799 | 245.776 | 659.411 | 199.764 |
| plaintext | 5 | 944.658 | 293.892 | 652.746 | 204.894 |
| plaintext | 6 | 734.102 | 212.639 | 639.685 | 208.561 |
| memory_tls | 0 | 1047.846 | 541.778 | 1000.729 | 519.736 |
| memory_tls | 1 | 1232.305 | 496.821 | 949.103 | 482.687 |
| memory_tls | 2 | 1229.466 | 570.890 | 964.969 | 519.992 |
| memory_tls | 3 | 1147.211 | 641.912 | 952.980 | 518.723 |
| memory_tls | 4 | 1129.402 | 486.804 | 948.861 | 474.847 |
| memory_tls | 5 | 1094.105 | 560.707 | 903.132 | 469.643 |
| memory_tls | 6 | 987.196 | 642.150 | 908.280 | 489.451 |
| ktls | 0 | 1135.351 | 627.783 | 805.374 | 392.494 |
| ktls | 1 | 882.874 | 526.611 | 868.005 | 418.944 |
| ktls | 2 | 929.997 | 560.093 | 805.097 | 425.895 |
| ktls | 3 | 971.081 | 478.393 | 793.607 | 412.105 |
| ktls | 4 | 1010.251 | 485.821 | 836.126 | 417.990 |
| ktls | 5 | 1065.492 | 656.784 | 853.670 | 430.227 |
| ktls | 6 | 913.643 | 460.332 | 875.246 | 431.108 |
