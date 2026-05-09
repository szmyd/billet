#!/usr/bin/env bash
# Sender engine sanity matrix vs fio. Read-only -- safe to point at a device
# with a live filesystem. Per-row gate: IOPS within +/-5%, p99 within +/-10%.

set -euo pipefail

DEVICE=""
DURATION=10
QDS=(1 4 16 32 64 128)
RESULTS_DIR=""
BILLET="${BILLET:-build/Release/src/cli/billet}"
FIO="${FIO:-fio}"

# billet is allowed to outperform fio (lower per-op overhead -> higher IOPS),
# but must not fall significantly behind. p99 is bidirectional: a real timing
# bug shows up as latency divergence in either direction.
IOPS_REGRESSION_TOL=5
P99_TOL=10

usage() {
    cat <<EOF
Usage: $0 --device <path> [--duration <s>] [--results-dir <dir>] [--qds <list>]

Sweeps a closed-loop 4K random-read across multiple queue depths and compares
billet's sender engine to fio. Read-only; safe on devices hosting live
filesystems.

Options:
  --device <path>      Block device to test (required)
  --duration <s>       Per-config run length in seconds (default: 10)
  --qds <list>         Comma-separated QD list (default: 1,4,16,32,64,128)
  --results-dir <dir>  Where to drop per-run JSON (default: /tmp/billet-smoke-<unix>)

Environment overrides:
  BILLET   path to the billet binary (default: build/Release/src/cli/billet)
  FIO      path to fio (default: looked up on PATH)

Tolerances: billet IOPS may not regress more than ${IOPS_REGRESSION_TOL}%
relative to fio (faster is OK); p99 must be within +/-${P99_TOL}% bidirectional.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device) DEVICE="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --qds) IFS=',' read -ra QDS <<< "$2"; shift 2 ;;
        --results-dir) RESULTS_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$DEVICE" ]]; then echo "--device is required" >&2; exit 2; fi
if [[ ! -b "$DEVICE" ]]; then echo "$DEVICE is not a block device" >&2; exit 2; fi
if ! command -v jq >/dev/null;     then echo "jq not found on PATH" >&2; exit 2; fi
if ! command -v "$FIO" >/dev/null; then echo "fio not found ('$FIO')" >&2; exit 2; fi
if [[ ! -x "$BILLET" ]];           then echo "billet binary not found at '$BILLET'" >&2; exit 2; fi

RESULTS_DIR="${RESULTS_DIR:-/tmp/billet-smoke-$(date +%s)}"
mkdir -p "$RESULTS_DIR"

run_billet() {
    local qd="$1"
    local out="$RESULTS_DIR/billet-qd${qd}.json"
    "$BILLET" --engine senders --device "$DEVICE" --profile random_read_4k \
              --workers 1 --duration "$DURATION" --qd "$qd" \
              --output "$out" >"$RESULTS_DIR/billet-qd${qd}.log" 2>&1
    local iops p99
    iops=$(jq -r '.results.summary.iops_mean' "$out")
    p99=$(jq -r '.results.by_op.Read.p99_us' "$out")
    echo "$iops $p99"
}

run_fio() {
    local qd="$1"
    local out="$RESULTS_DIR/fio-qd${qd}.json"
    "$FIO" --name=smoke --filename="$DEVICE" \
        --rw=randread --bs=4k --iodepth="$qd" --ioengine=io_uring --direct=1 \
        --time_based=1 --runtime="$DURATION" \
        --norandommap=1 --random_distribution=random \
        --numjobs=1 --group_reporting=1 \
        --output-format=json --output="$out" >"$RESULTS_DIR/fio-qd${qd}.log" 2>&1
    local iops p99_ns p99
    iops=$(jq -r '.jobs[0].read.iops' "$out")
    p99_ns=$(jq -r '.jobs[0].read.clat_ns.percentile."99.000000"' "$out")
    p99=$(awk -v n="$p99_ns" 'BEGIN{printf "%.0f", n/1000}')
    echo "$iops $p99"
}

abs_pct() {
    awk -v a="$1" -v b="$2" 'BEGIN{ if (a==0){print 0; exit} d=(b-a)/a*100; print (d<0?-d:d) }'
}

# Signed delta of b vs a, in percent. Negative -> b is below a.
signed_pct() {
    awk -v a="$1" -v b="$2" 'BEGIN{ if (a==0){print 0; exit} print (b-a)/a*100 }'
}

printf "device:   %s\n"   "$DEVICE"
printf "duration: %ss\n"  "$DURATION"
printf "results:  %s\n\n" "$RESULTS_DIR"

printf "%-5s %-13s %-13s %-10s %-13s %-12s %-9s %s\n" \
    "qd" "billet iops" "fio iops" "diops%" "billet p99us" "fio p99us" "dp99%" "result"
printf '%.0s-' {1..100}; echo

overall_pass=1

for qd in "${QDS[@]}"; do
    read -r b_iops b_p99 < <(run_billet "$qd")
    read -r f_iops f_p99 < <(run_fio "$qd")

    iops_signed=$(signed_pct "$f_iops" "$b_iops")
    p99_delta=$(abs_pct "$f_p99" "$b_p99")

    result="PASS"
    # Fail if billet is more than IOPS_REGRESSION_TOL% slower than fio
    # (signed delta < -IOPS_REGRESSION_TOL).
    if awk -v d="$iops_signed" -v t="$IOPS_REGRESSION_TOL" 'BEGIN{exit !(d < -t)}'; then
        result="FAIL"; overall_pass=0
    fi
    if awk -v d="$p99_delta" -v t="$P99_TOL" 'BEGIN{exit !(d>t)}'; then
        result="FAIL"; overall_pass=0
    fi

    printf "%-5s %-13.1f %-13.1f %+9.2f%% %-13.0f %-12.0f %-9.2f %s\n" \
        "$qd" "$b_iops" "$f_iops" "$iops_signed" "$b_p99" "$f_p99" "$p99_delta" "$result"
done

echo
if [[ "$overall_pass" -eq 1 ]]; then
    echo "OVERALL: PASS  (gate: billet iops >= fio - ${IOPS_REGRESSION_TOL}%, p99 within +/-${P99_TOL}%)"
    exit 0
else
    echo "OVERALL: FAIL  (gate: billet iops >= fio - ${IOPS_REGRESSION_TOL}%, p99 within +/-${P99_TOL}%)"
    exit 1
fi
