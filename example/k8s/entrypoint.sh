#!/usr/bin/env bash
# Default entrypoint for a billet runner pod. Runs the scripted
# profile series against a block device exposed via the pod's
# volumeDevices, drops each run's JSON + a sidecar metadata.json into
# $OUTPUT_DIR, tar.gz's the lot, and echoes the audit blob to stdout
# so a stdout-tailing log scraper picks it up even without
# kubectl cp.
#
# Environment (manifest-supplied unless noted):
#   DEVICE_PATH       block device exposed via volumeDevices. Default /dev/billet-target.
#   DEVICE_LABEL      identifies the scenario; prefixes the metrics entity. Default "unlabeled".
#   TARGET_PVC        PVC name that backs the block device, recorded for audit.
#   OUTPUT_DIR        where billet writes JSON + the archive. Default /output.
#   DURATION_S        per-profile duration. Default 120.
#   QD                per-worker workload_ctx qd cap. Default 32.
#   WORKERS           pinned worker threads; 0 = one per NUMA-local hw queue. Default 1.
#   PIN_STRATEGY      worker CPU pinning: auto|mq|numa|linear|none. Default numa
#                     (device-local placement; stable across the matrix nodes).
#   METRICS_PORT      Prometheus exposition port. Default 9777.
#   METRICS_DRAIN_S   seconds /metrics stays up post-run for a final scrape. Default 30.
#   JOB_NAME / POD_NAME / POD_NAMESPACE / NODE_NAME -- downward API; recorded for audit.

set -uo pipefail

DEVICE_PATH="${DEVICE_PATH:-/dev/billet-target}"
OUTPUT_DIR="${OUTPUT_DIR:-/output}"
DURATION_S="${DURATION_S:-120}"
QD="${QD:-32}"
WORKERS="${WORKERS:-1}"
PIN_STRATEGY="${PIN_STRATEGY:-numa}"
METRICS_PORT="${METRICS_PORT:-9777}"
METRICS_DRAIN_S="${METRICS_DRAIN_S:-30}"
DEVICE_LABEL="${DEVICE_LABEL:-unlabeled}"

JOB_NAME="${JOB_NAME:-}"
POD_NAME="${POD_NAME:-}"
POD_NAMESPACE="${POD_NAMESPACE:-}"
NODE_NAME="${NODE_NAME:-}"
TARGET_PVC="${TARGET_PVC:-}"

mkdir -p "$OUTPUT_DIR"

START_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
START_EPOCH_S="$(date -u +%s)"

# Host facts visible to the pod. These don't change for the run, so
# capture once. /proc/cpuinfo "model name" works on x86; on ARM the
# field may be absent and CPU_MODEL ends up empty -- acceptable.
# `nproc` respects cgroup cpu.max in modern kernels, so it reflects
# the pod's effective parallelism, not the host's full core count.
KERNEL_VERSION="$(uname -r 2>/dev/null || echo '')"
CPU_MODEL_RAW="$(awk -F: '/^model name/ {sub(/^[ \t]+/, "", $2); print $2; exit}' /proc/cpuinfo 2>/dev/null || echo '')"
# JSON-safe: escape any embedded double quotes in the cpu model string.
CPU_MODEL="${CPU_MODEL_RAW//\"/\\\"}"
CPU_COUNT_VISIBLE="$(nproc 2>/dev/null || echo 0)"

# Capture billet identity for audit. Failure here doesn't abort the
# run -- the JSONs and the metadata are still useful.
billet --version > "$OUTPUT_DIR/billet-version.txt" 2>&1 || true

write_metadata() {
    local status="$1"
    local completed_at="${2:-}"
    local end_epoch_s="${3:-0}"
    cat > "$OUTPUT_DIR/metadata.json" <<EOF
{
  "schema_version": "billet.k8s-run/1",
  "device_label": "${DEVICE_LABEL}",
  "device_path": "${DEVICE_PATH}",
  "job_name": "${JOB_NAME}",
  "pod_name": "${POD_NAME}",
  "pod_namespace": "${POD_NAMESPACE}",
  "node_name": "${NODE_NAME}",
  "pvc_name": "${TARGET_PVC}",
  "duration_s": ${DURATION_S},
  "qd": ${QD},
  "workers": ${WORKERS},
  "pin_strategy": "${PIN_STRATEGY}",
  "metrics_port": ${METRICS_PORT},
  "metrics_drain_s": ${METRICS_DRAIN_S},
  "started_at": "${START_TS}",
  "start_epoch_s": ${START_EPOCH_S},
  "completed_at": "${completed_at}",
  "end_epoch_s": ${end_epoch_s},
  "kernel_version": "${KERNEL_VERSION}",
  "cpu_model": "${CPU_MODEL}",
  "cpu_count_visible_to_pod": ${CPU_COUNT_VISIBLE},
  "status": "${status}",
  "scenarios_attempted": ["random_read_4k", "postgresql"]
}
EOF
}

write_metadata "running" "" 0

echo "[billet-runner] profile=random_read_4k label=${DEVICE_LABEL}"
billet \
    --device "$DEVICE_PATH" \
    --device-label "$DEVICE_LABEL" \
    --profile random_read_4k \
    --workers "$WORKERS" --pin-strategy "$PIN_STRATEGY" --qd "$QD" --duration "$DURATION_S" \
    --metrics-port "$METRICS_PORT" \
    --metrics-drain-s "$METRICS_DRAIN_S" \
    --output "$OUTPUT_DIR/${DEVICE_LABEL}-rr4k.json"
rr4k_status=$?

echo "[billet-runner] profile=postgresql label=${DEVICE_LABEL}"
billet \
    --device "$DEVICE_PATH" \
    --device-label "$DEVICE_LABEL" \
    --profile postgresql \
    --workers "$WORKERS" --pin-strategy "$PIN_STRATEGY" --qd "$QD" --duration "$DURATION_S" \
    --metrics-port "$METRICS_PORT" \
    --metrics-drain-s "$METRICS_DRAIN_S" \
    --allow-destructive \
    --output "$OUTPUT_DIR/${DEVICE_LABEL}-pg.json"
pg_status=$?

END_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
END_EPOCH_S="$(date -u +%s)"
overall="ok"
if [ "$rr4k_status" -ne 0 ] || [ "$pg_status" -ne 0 ]; then
    overall="partial"
fi
write_metadata "$overall" "$END_TS" "$END_EPOCH_S"

# Archive for one-shot kubectl cp.
tar -C "$OUTPUT_DIR" -czf "$OUTPUT_DIR/results.tar.gz" \
    metadata.json billet-version.txt \
    "${DEVICE_LABEL}-rr4k.json" "${DEVICE_LABEL}-pg.json" 2>/dev/null || \
tar -C "$OUTPUT_DIR" -czf "$OUTPUT_DIR/results.tar.gz" .

echo "[billet-runner] metadata:"
cat "$OUTPUT_DIR/metadata.json"
echo "[billet-runner] results archive at $OUTPUT_DIR/results.tar.gz"

# Exit nonzero only if every profile failed -- partial success is
# still worth collecting.
if [ "$rr4k_status" -ne 0 ] && [ "$pg_status" -ne 0 ]; then
    exit 1
fi
exit 0
