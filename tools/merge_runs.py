#!/usr/bin/env python3
"""billet-merge: combine two billet.run/1 JSONs into one.

Adds counts, bytes, iops, and throughput. Merges per-percentile latencies
via count-weighted average; max takes the higher of the two. The HDR
histogram blobs are dropped from the output -- compare.py does not consume
them and they cannot be meaningfully merged without re-running the HDR codec.

Usage:
    tools/merge_runs.py q0.json q1.json -o combined.json --label nb_2.0-q2
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

PCT_KEYS = ["p50_us", "p99_us", "p99_9_us", "p99_99_us"]


def merge_cell(a: dict, b: dict) -> dict:
    na, nb = a.get("count", 0), b.get("count", 0)
    n = na + nb
    out: dict = {"count": n, "bytes": a.get("bytes", 0) + b.get("bytes", 0)}
    if n > 0:
        for p in PCT_KEYS:
            out[p] = int(round((na * a.get(p, 0) + nb * b.get(p, 0)) / n))
        out["max_us"] = max(a.get("max_us", 0), b.get("max_us", 0))
    else:
        for p in PCT_KEYS:
            out[p] = 0
        out["max_us"] = 0
    return out


def merge_cell_maps(a: dict, b: dict) -> dict:
    return {k: merge_cell(a.get(k, {}), b.get(k, {})) for k in set(a) | set(b)}


def merge(a: dict, b: dict, label: str) -> dict:
    ra, rb = a["results"], b["results"]
    sa, sb = ra["summary"], rb["summary"]
    out = dict(a)
    out["device"] = dict(a["device"])
    out["device"]["label"] = label
    out["engine"] = dict(a["engine"])
    out["engine"]["workers"] = a["engine"].get("workers", 1) + b["engine"].get("workers", 1)
    out["engine"].pop("pin_cpu", None)
    out["results"] = {
        "summary": {
            "ops_total":       sa["ops_total"]       + sb["ops_total"],
            "bytes_total":     sa["bytes_total"]      + sb["bytes_total"],
            "iops_mean":       sa["iops_mean"]        + sb["iops_mean"],
            "throughput_mibs": sa["throughput_mibs"]  + sb["throughput_mibs"],
            "errors":          sa["errors"]           + sb["errors"],
            "component_drops": sa["component_drops"]  + sb["component_drops"],
        },
        "by_component": merge_cell_maps(ra.get("by_component", {}), rb.get("by_component", {})),
        "by_op":        merge_cell_maps(ra.get("by_op", {}),        rb.get("by_op", {})),
        "by_phase": {},
    }
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("a", type=Path, help="First billet.run/1 JSON")
    parser.add_argument("b", type=Path, help="Second billet.run/1 JSON")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output path")
    parser.add_argument("--label", default="", help="device.label for the merged run (default: output stem)")
    args = parser.parse_args()

    label = args.label or args.output.stem

    a = json.loads(args.a.read_text())
    b = json.loads(args.b.read_text())

    if a.get("schema_version") != "billet.run/1" or b.get("schema_version") != "billet.run/1":
        print("error: both inputs must be billet.run/1 JSON files")
        return 1
    if a["profile"]["name"] != b["profile"]["name"]:
        print(f"warning: profile mismatch ({a['profile']['name']} vs {b['profile']['name']})")

    merged = merge(a, b, label)
    args.output.write_text(json.dumps(merged, indent=2))
    print(f"wrote {args.output}  (label={label})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
