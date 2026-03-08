#!/bin/bash
# ---------------------------------------------------------------------------
# compare.sh — Compare two benchmark JSON outputs side-by-side.
#
# Usage:
#   ./benchmarks/compare.sh <baseline.json> <candidate.json> [--filter=PATTERN]
#
# Output:
#   Formatted table showing median times, items/s, and delta (%) for each
#   benchmark present in both runs. Benchmarks only in one run are listed
#   separately.
#
# Requires python3.
# ---------------------------------------------------------------------------

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <baseline.json> <candidate.json> [--filter=PATTERN]" >&2
    exit 1
fi

BASELINE="$1"
CANDIDATE="$2"
FILTER=""

shift 2
while [[ $# -gt 0 ]]; do
    case "$1" in
        --filter=*) FILTER="${1#*=}" ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

if ! command -v python3 &>/dev/null; then
    echo "error: python3 required" >&2
    exit 1
fi

# Warn if filenames suggest different architectures. Filenames from capture.sh
# embed arch: YYYYMMDD_HHMMSS_<arch>_<rev>[_label].json
#
ARCH_BASE=$(basename "$BASELINE" | sed -n 's/^[0-9]*_[0-9]*_\([^_]*\)_.*/\1/p')
ARCH_CAND=$(basename "$CANDIDATE" | sed -n 's/^[0-9]*_[0-9]*_\([^_]*\)_.*/\1/p')
if [[ -n "$ARCH_BASE" && -n "$ARCH_CAND" && "$ARCH_BASE" != "$ARCH_CAND" ]]; then
    echo "WARNING: architecture mismatch — baseline=$ARCH_BASE, candidate=$ARCH_CAND" >&2
    echo "  Cross-architecture comparisons are informational only, not regression signals." >&2
    echo "" >&2
fi

python3 - "$BASELINE" "$CANDIDATE" "$FILTER" << 'PYSCRIPT'
import json, sys, re

def load_medians(path):
    with open(path) as f:
        data = json.load(f)
    benchmarks = data.get("benchmarks", [])
    medians = [b for b in benchmarks if b.get("aggregate_name") == "median"]
    if not medians:
        medians = [b for b in benchmarks if "aggregate_name" not in b]
    result = {}
    for b in medians:
        name = b["name"].replace("_median", "")
        result[name] = b
    return result

baseline_path = sys.argv[1]
candidate_path = sys.argv[2]
filter_pattern = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None

base = load_medians(baseline_path)
cand = load_medians(candidate_path)

all_names = sorted(set(list(base.keys()) + list(cand.keys())))

if filter_pattern:
    pat = re.compile(filter_pattern)
    all_names = [n for n in all_names if pat.search(n)]

both = [n for n in all_names if n in base and n in cand]
base_only = [n for n in all_names if n in base and n not in cand]
cand_only = [n for n in all_names if n not in base and n in cand]

def fmt_ns(v):
    if v >= 1_000_000_000:
        return f"{v/1e9:.2f}s"
    elif v >= 1_000_000:
        return f"{v/1e6:.2f}ms"
    elif v >= 1_000:
        return f"{v/1e3:.1f}us"
    else:
        return f"{v:.0f}ns"

def fmt_rate(v):
    if not v:
        return "-"
    if v >= 1_000_000:
        return f"{v/1e6:.2f}M/s"
    elif v >= 1_000:
        return f"{v/1e3:.1f}K/s"
    else:
        return f"{v:.0f}/s"

def delta_str(old, new):
    if old == 0:
        return "n/a"
    pct = ((new - old) / old) * 100
    sign = "+" if pct >= 0 else ""
    # For time: negative = improvement. For rate: positive = improvement.
    return f"{sign}{pct:.1f}%"

if both:
    # Header.
    #
    name_w = max(len(n) for n in both)
    name_w = max(name_w, 9)

    print(f"{'Benchmark':<{name_w}}  {'Base Time':>12}  {'Cand Time':>12}  {'Delta':>8}"
          f"  {'Base Rate':>12}  {'Cand Rate':>12}  {'Delta':>8}")
    print(f"{'-'*name_w}  {'-'*12}  {'-'*12}  {'-'*8}  {'-'*12}  {'-'*12}  {'-'*8}")

    for name in both:
        b = base[name]
        c = cand[name]
        bt = b.get("real_time", 0)
        ct = c.get("real_time", 0)
        br = b.get("items_per_second", 0)
        cr = c.get("items_per_second", 0)

        td = delta_str(bt, ct)
        rd = delta_str(br, cr) if br else "-"

        # Color-code: green if time decreased, red if increased.
        #
        if bt > 0 and ct > 0:
            pct = ((ct - bt) / bt) * 100
            if pct < -2:
                td = f"\033[32m{td}\033[0m"  # green = faster
            elif pct > 2:
                td = f"\033[31m{td}\033[0m"  # red = slower

        if br > 0 and cr > 0:
            pct = ((cr - br) / br) * 100
            if pct > 2:
                rd = f"\033[32m{rd}\033[0m"  # green = higher rate
            elif pct < -2:
                rd = f"\033[31m{rd}\033[0m"  # red = lower rate

        print(f"{name:<{name_w}}  {fmt_ns(bt):>12}  {fmt_ns(ct):>12}  {td:>8}"
              f"  {fmt_rate(br):>12}  {fmt_rate(cr):>12}  {rd:>8}")

if base_only:
    print(f"\nBaseline only ({len(base_only)}):")
    for n in base_only:
        print(f"  {n}")

if cand_only:
    print(f"\nCandidate only ({len(cand_only)}):")
    for n in cand_only:
        print(f"  {n}")

if not both and not base_only and not cand_only:
    print("No matching benchmarks found.")
PYSCRIPT
