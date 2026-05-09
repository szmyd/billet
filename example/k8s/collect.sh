#!/usr/bin/env bash
# Pull the results archive out of a completed billet runner pod and
# augment the audit metadata with the PV name (only the cluster API
# can join PVC -> PV).
#
# Usage:
#   collect.sh <job-name> [destination-dir] [namespace]
#
# Requires:
#   - `kubectl cp` against a Completed pod from the named Job.
#   - `get pvc` permissions in the namespace for PV resolution; if
#     not granted, the script still collects but leaves pv_name blank.

set -euo pipefail

JOB="${1:?usage: $0 <job-name> [destination-dir] [namespace]}"
DEST="${2:-./collected/$JOB}"
NAMESPACE="${3:-default}"

mkdir -p "$DEST"

POD=$(kubectl -n "$NAMESPACE" get pods -l job-name="$JOB" \
    -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || true)
if [ -z "$POD" ]; then
    echo "no pod found for job=$JOB in namespace=$NAMESPACE" >&2
    exit 1
fi

echo "collecting from pod=$POD"
kubectl -n "$NAMESPACE" cp "$POD:/output/results.tar.gz" "$DEST/results.tar.gz"
tar -xzf "$DEST/results.tar.gz" -C "$DEST"

# Augment metadata.json with the PV name resolved from the PVC.
PVC=$(jq -r '.pvc_name // empty' "$DEST/metadata.json" 2>/dev/null || echo "")
if [ -n "$PVC" ]; then
    PV=$(kubectl -n "$NAMESPACE" get pvc "$PVC" \
        -o jsonpath='{.spec.volumeName}' 2>/dev/null || echo "")
    if [ -n "$PV" ]; then
        jq --arg pv "$PV" '. + {pv_name: $pv}' "$DEST/metadata.json" \
            > "$DEST/metadata.json.tmp"
        mv "$DEST/metadata.json.tmp" "$DEST/metadata.json"
        echo "resolved PV: $PV"
    else
        echo "warning: could not resolve PV for PVC=$PVC (RBAC?)"
    fi
fi

echo "done: $DEST"
ls -la "$DEST"
