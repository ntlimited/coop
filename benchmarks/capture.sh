#!/bin/bash
# ---------------------------------------------------------------------------
# capture.sh — Run benchmarks with full environment context capture.
#
# Usage:
#   ./benchmarks/capture.sh [options]
#
# Options:
#   --filter=PATTERN    Google Benchmark filter (regex)
#   --label=NAME        Human label for this run (e.g., "bswap_key16")
#   --reps=N            Repetitions per benchmark (default: 3)
#   --min-time=SECS     Minimum time per benchmark (default: gbench default)
#   --output-dir=DIR    Override output directory
#   --no-build          Skip release build step
#   --json-only         Only emit JSON, skip markdown report
#   --quiet             Suppress gbench console output
#
# Output:
#   benchmarks/reports/runs/<timestamp>_<arch>_<gitrev>[_<label>].json
#   benchmarks/reports/runs/<timestamp>_<arch>_<gitrev>[_<label>].md
#
# The markdown report includes full environment context (hardware, load,
# kernel, git state) and a formatted results table extracted from the JSON.
# ---------------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

REPORTS_DIR="$SCRIPT_DIR/reports"
RUNS_DIR="$REPORTS_DIR/runs"

# Defaults.
#
FILTER=""
LABEL=""
REPS=3
MIN_TIME=""
OUTPUT_DIR=""
NO_BUILD=0
JSON_ONLY=0
QUIET=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --filter=*)   FILTER="${1#*=}" ;;
        --label=*)    LABEL="${1#*=}" ;;
        --reps=*)     REPS="${1#*=}" ;;
        --min-time=*) MIN_TIME="${1#*=}" ;;
        --output-dir=*) OUTPUT_DIR="${1#*=}" ;;
        --no-build)   NO_BUILD=1 ;;
        --json-only)  JSON_ONLY=1 ;;
        --quiet)      QUIET=1 ;;
        -h|--help)
            sed -n '2,/^# ----/{ /^# ----/d; s/^# \?//p }' "$0"
            exit 0
            ;;
        *)
            echo "Unknown option: $1 (try --help)" >&2
            exit 1
            ;;
    esac
    shift
done

[[ -n "$OUTPUT_DIR" ]] && RUNS_DIR="$OUTPUT_DIR"

# ---------------------------------------------------------------------------
# Build release if needed.
# ---------------------------------------------------------------------------

BENCH_BIN="$PROJECT_ROOT/build/release/bin/coop_benchmarks"

if [[ $NO_BUILD -eq 0 ]]; then
    if [[ ! -f "$PROJECT_ROOT/build/release/CMakeCache.txt" ]]; then
        echo "==> Configuring release build..."
        cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build/release" \
              -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
              >/dev/null 2>&1
    fi
    echo "==> Building coop_benchmarks..."
    if ! cmake --build "$PROJECT_ROOT/build/release" --target coop_benchmarks \
               -j"$(nproc)" 2>&1; then
        echo "error: release build failed" >&2
        exit 1
    fi
fi

if [[ ! -x "$BENCH_BIN" ]]; then
    echo "error: benchmark binary not found: $BENCH_BIN" >&2
    echo "Run without --no-build or build manually." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Gather environment context.
# ---------------------------------------------------------------------------

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
TIMESTAMP_ISO=$(date -Iseconds)
GIT_REV=$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_DIRTY=""
if ! git -C "$PROJECT_ROOT" diff --quiet 2>/dev/null; then
    GIT_DIRTY="-dirty"
fi
GIT_BRANCH=$(git -C "$PROJECT_ROOT" branch --show-current 2>/dev/null || echo "detached")

ARCH=$(uname -m)
KERNEL=$(uname -r)

# CPU info — portable across Linux variants. Pipelines can fail (no match from
# grep) under pipefail, so || true each one.
#
CPU_MODEL=$(lscpu 2>/dev/null | grep -i "model name" | sed 's/.*: *//' | head -1 || true)
[[ -z "$CPU_MODEL" ]] && CPU_MODEL="unknown"
CPU_CORES=$(nproc 2>/dev/null || echo "?")
CPU_MHZ=$(lscpu 2>/dev/null | grep -i "CPU max MHz" | sed 's/.*: *//' | head -1 || true)
[[ -z "$CPU_MHZ" ]] && CPU_MHZ=$(lscpu 2>/dev/null | grep -i "CPU MHz" | sed 's/.*: *//' | head -1 || true)

# Cache hierarchy.
#
CACHE_INFO=$(lscpu 2>/dev/null | grep -i "cache" | sed 's/.*: *//' | paste -sd', ' || true)

# Memory.
#
MEM_TOTAL=$(free -h 2>/dev/null | awk '/Mem:/{print $2}' || echo "?")
MEM_AVAIL=$(free -h 2>/dev/null | awk '/Mem:/{print $7}' || echo "?")

# Load averages (pre-run).
#
LOAD_PRE=$(cat /proc/loadavg 2>/dev/null | cut -d' ' -f1-3 || echo "?")

# ---------------------------------------------------------------------------
# Prepare output.
# ---------------------------------------------------------------------------

mkdir -p "$RUNS_DIR"

RUN_NAME="${TIMESTAMP}_${ARCH}_${GIT_REV}${GIT_DIRTY}"
[[ -n "$LABEL" ]] && RUN_NAME="${RUN_NAME}_${LABEL}"
JSON_OUT="$RUNS_DIR/${RUN_NAME}.json"

# ---------------------------------------------------------------------------
# Build benchmark arguments.
# ---------------------------------------------------------------------------

BENCH_ARGS=(
    --benchmark_format=json
    --benchmark_out="$JSON_OUT"
    --benchmark_repetitions="$REPS"
)

[[ -n "$FILTER" ]]   && BENCH_ARGS+=(--benchmark_filter="$FILTER")
[[ -n "$MIN_TIME" ]] && BENCH_ARGS+=(--benchmark_min_time="$MIN_TIME")

# ---------------------------------------------------------------------------
# Print run header.
# ---------------------------------------------------------------------------

echo "=== Benchmark Capture ==="
echo "  Git:    ${GIT_REV}${GIT_DIRTY} (${GIT_BRANCH})"
echo "  CPU:    ${CPU_MODEL} (${CPU_CORES} cores)"
echo "  Arch:   ${ARCH}"
echo "  Memory: ${MEM_TOTAL} total, ${MEM_AVAIL} available"
echo "  Kernel: ${KERNEL}"
echo "  Load:   ${LOAD_PRE}"
[[ -n "$FILTER" ]] && echo "  Filter: ${FILTER}"
[[ -n "$LABEL" ]]  && echo "  Label:  ${LABEL}"
echo ""

# ---------------------------------------------------------------------------
# Run benchmarks.
# ---------------------------------------------------------------------------

BENCH_EXIT=0
if [[ $QUIET -eq 1 ]]; then
    "$BENCH_BIN" "${BENCH_ARGS[@]}" --benchmark_format=json >/dev/null 2>&1 || BENCH_EXIT=$?
else
    "$BENCH_BIN" "${BENCH_ARGS[@]}" 2>&1 || BENCH_EXIT=$?
fi

if [[ $BENCH_EXIT -ne 0 ]]; then
    echo ""
    echo "  WARNING: benchmark exited with code $BENCH_EXIT"
fi

if [[ ! -f "$JSON_OUT" ]]; then
    echo "error: benchmark did not produce output at $JSON_OUT" >&2
    exit 1
fi

# Post-run load.
#
LOAD_POST=$(cat /proc/loadavg 2>/dev/null | cut -d' ' -f1-3 || echo "?")

echo ""
echo "  Load (post): ${LOAD_POST}"
echo "  JSON: ${JSON_OUT}"

# ---------------------------------------------------------------------------
# Generate markdown report.
# ---------------------------------------------------------------------------

if [[ $JSON_ONLY -eq 1 ]]; then
    exit 0
fi

REPORT_OUT="$RUNS_DIR/${RUN_NAME}.md"

# Header.
#
cat > "$REPORT_OUT" << HEADER
# Benchmark Run: ${RUN_NAME}

## Environment

| Property | Value |
|----------|-------|
| Timestamp | ${TIMESTAMP_ISO} |
| Git revision | \`${GIT_REV}${GIT_DIRTY}\` |
| Git branch | \`${GIT_BRANCH}\` |
| Architecture | ${ARCH} |
| CPU | ${CPU_MODEL} |
| Cores | ${CPU_CORES} |
| CPU MHz | ${CPU_MHZ:-n/a} |
| Caches | ${CACHE_INFO:-n/a} |
| Memory (total) | ${MEM_TOTAL} |
| Memory (available) | ${MEM_AVAIL} |
| Kernel | ${KERNEL} |
| Load avg (pre) | ${LOAD_PRE} |
| Load avg (post) | ${LOAD_POST} |
HEADER

if [[ -n "$FILTER" ]]; then
    echo "| Filter | \`${FILTER}\` |" >> "$REPORT_OUT"
fi

echo "" >> "$REPORT_OUT"

# Results table — extract medians (or raw if no repetitions) from JSON.
#
python3 - "$JSON_OUT" >> "$REPORT_OUT" 2>/dev/null << 'PYSCRIPT'
import json, sys

with open(sys.argv[1]) as f:
    data = json.load(f)

benchmarks = data.get("benchmarks", [])
if not benchmarks:
    print("*No benchmark results.*")
    sys.exit(0)

# Prefer median aggregates; fall back to raw results.
#
medians = [b for b in benchmarks if b.get("aggregate_name") == "median"]
if not medians:
    medians = [b for b in benchmarks if "aggregate_name" not in b]

# Also collect CV for variability assessment.
#
cvs = {}
for b in benchmarks:
    if b.get("aggregate_name") == "cv":
        name = b["name"].rsplit("_cv", 1)[0]
        cvs[name] = b.get("real_time", 0)

# Group by prefix (everything before the first /).
#
groups = {}
for b in medians:
    name = b["name"]
    clean = name.replace("_median", "")
    prefix = clean.split("/")[0]
    if prefix not in groups:
        groups[prefix] = []
    groups[prefix].append((clean, b))

print("## Results\n")
for prefix, entries in groups.items():
    print(f"### {prefix}\n")
    print("| Benchmark | Time (ns) | CPU (ns) | Iterations | Items/s | CV% |")
    print("|-----------|-----------|----------|------------|---------|-----|")
    for clean, b in entries:
        time_ns = f"{b.get('real_time', 0):.0f}"
        cpu_ns = f"{b.get('cpu_time', 0):.0f}"
        iters = b.get("iterations", "")
        items = b.get("items_per_second", 0)
        items_str = f"{items:,.0f}" if items else "-"
        cv_key = clean
        cv = cvs.get(cv_key, None)
        cv_str = f"{cv:.1f}" if cv is not None else "-"
        short = clean[len(prefix):].lstrip("/") or prefix
        print(f"| {short} | {time_ns} | {cpu_ns} | {iters} | {items_str} | {cv_str} |")
    print()
PYSCRIPT

# If python3 not available, note it.
#
if [[ $? -ne 0 ]]; then
    echo "## Results" >> "$REPORT_OUT"
    echo "" >> "$REPORT_OUT"
    echo "*python3 required for formatted table. Raw JSON: \`${JSON_OUT}\`*" >> "$REPORT_OUT"
fi

echo "  Report: ${REPORT_OUT}"
