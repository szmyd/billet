# billet vs fio for nublox Performance Testing

billet: function generator and oscilloscope for block devices.

This brief argues for replacing the fio-based performance tests in nublox-verify
with billet, specifically for the jobs that measure storage throughput and latency.
It is not an argument against fio generally — fio's data-integrity verification
mode is irreplaceable and should stay. The argument is targeted at the performance
sweep and longevity tests.

---

## What nublox-verify actually does with fio

### Pattern A — parameter sweep (`performance_raid0.py`)

```python
# performance_raid0.py (simplified)
for test_type in ["randread", "randwrite", "randrw", "read", "write"]:
    for bs in ["4k", "128k"]:
        for iodepth in [1, 2, 4, 8, 16, 32, 64]:
            run_fio(test_type, bs, iodepth, runtime=300)
```

5 types × 2 block sizes × 7 queue depths × 300 s = roughly 6 hours of wall time
per storage configuration. Output is pod logs in whatever format fio emits.

### Pattern B — long-running mixed write (`performance_raid10.py`, `longevity.py`)

```python
# verify.py — create_fio_pod() (critical path, simplified)
entrypoint += f"cd {first_mount_point} ; fio {fio_args} 2>&1"
```

`create_fio_pod` mounts the PVC as a filesystem and runs fio inside the mount
point directory. `stress_test` uses `numjobs=10`, default `iodepth=16`, runtime up
to 17 000 s. Longevity tests run 48–168 h.

---

## Problem 1: They're testing the filesystem, not nublox

The single most important fact: **fio runs inside a filesystem mount**.

```
Pod → VFS → ext4/xfs → nublox block driver
                 ↑
         fio sees this layer
```

fio's `direct=1` flag bypasses the page cache, but it does not bypass the
filesystem. Every `write` goes through the VFS layer, through the journal, through
the block allocator, and only then reaches the nublox device. `read` performance is
similarly distorted by read-ahead, extent mapping, and directory lookups.

billet opens the block device with `O_DIRECT | O_RDWR` and issues `io_uring` SQEs
directly to the device node:

```
Pod → io_uring → nublox block driver
                      ↑
               billet sees this layer
```

If the goal is to measure nublox, the access mode matters as much as the workload
shape. A 10% regression in nublox's read path can be entirely hidden by filesystem
read-ahead. A 20% improvement in nublox's write path can be cancelled by a slow
fsync in the journal layer.

---

## Problem 2: fio's latency numbers are wrong under load

fio uses closed-loop `libaio` with a fixed `iodepth`. When the device slows down,
fio simply queues fewer ops — it never measures the ops that *should have been
issued* but were not yet because the previous ones hadn't completed.

This is the coordinated omission problem. At high queue depths and under
degradation, fio's p99 latency can be 2–10× lower than the actual latency a
real client would observe, because a real client has arrivals independent of
completions.

billet uses an open-loop Poisson emitter for workloads that model sustained
arrival rates. The emitter sets `intended_ts_ns` to the *scheduled arrival time*,
not the submission time:

```cpp
// poisson_emitter: scheduled arrival, not submission
o.intended_ts_ns = next_arrival_ns;   // when this op was supposed to start
co_await ctx.submit_op(o);            // may queue behind prior work
```

Latency = `completion_ts_ns - intended_ts_ns`. If an op had to wait 40 ms in the
QD gate because the device was saturated, those 40 ms appear in the histogram. fio
would report only the device service time; billet reports the total experience
latency.

---

## Problem 3: Sweeping parameters ≠ measuring workloads

The sweep in `performance_raid0.py` tells you the performance *envelope*: maximum
IOPS at each queue depth, maximum throughput at each block size. That is useful for
initial device characterization. It does not tell you how nublox behaves under a
workload that looks like what customers actually run.

billet's profiles model application I/O patterns rather than synthetic
single-operation sweeps:

| Profile | What it models |
|---|---|
| `random_read_4k` | cache-miss read from an OLTP working set |
| `postgresql` | 4 concurrent components: random reader, random writer, WAL write+fsync, checkpoint |

The PostgreSQL profile runs all four components simultaneously on the same device,
with realistic inter-arrival rates and fire-and-drain fsync semantics. No single
fio job in nublox-verify comes close to this: fio's `randrw` mixes reads and writes
but does not model the WAL pattern (batched writes followed by a mandatory fsync
before acknowledgement) that actually determines commit latency.

---

## Problem 4: No per-component breakdown

fio reports aggregate IOPS and latency for a job. When you run `numjobs=10` with
`randrw`, you get one number. You cannot see:

- how much of the latency is reads vs writes
- whether the write component degraded independently of the read component
- which component is the bottleneck when the aggregate drops

billet's `cell_layout` assigns every op a `component_id`. The `billet.run/1` JSON
carries per-component HDR histograms with full percentile fidelity:

```json
"by_component": {
  "reader":      { "p50_us": 180, "p99_us": 420,  "p999_us": 1100 },
  "rand_writer": { "p50_us": 210, "p99_us": 580,  "p999_us": 2400 },
  "wal":         { "p50_us": 380, "p99_us": 1200, "p999_us": 8100 },
  "ckpt":        { "p50_us": 950, "p99_us": 4200, "p999_us": 22000 }
}
```

When a nublox regression lands, you can see immediately whether it hit the random
reader, the WAL writer, or the checkpoint path. The fio aggregate hides this.

---

## Problem 5: No structured output; no live telemetry

fio writes text to stdout. nublox-verify collects it as pod logs. Comparing two
runs requires manual inspection or bespoke log parsing.

billet writes `billet.run/1` JSON to stdout. `tools/compare.py` reads N of those
JSONs and produces a self-contained HTML report with bar charts, latency spectrum
polylines, and a p99-vs-reference heat grid — no extra tooling required.

The Tess Job YAML already carries Sherlock annotations:

```yaml
annotations:
  io.sherlock.metrics/module: prometheus
  io.sherlock.metrics/hosts: "${data.host}:9777/metrics"
  io.sherlock.metrics/namespace: billet
  io.sherlock.metrics/period: 60s
```

Prometheus metrics update every 10 s during the run. With fio, you have no live
view; you wait for the pod to finish and parse the log. With billet, every storage
configuration produces a live Sherlock dashboard while the test runs.

---

## What fio still does well

fio's `verify=crc32c` with `verify_backlog` is the right tool for data integrity
testing. It writes a known pattern, reads it back, and checksums every byte.
billet does not implement content verification. The `write_verify` and
`verify_download` profiles in `fio_profiles.json` should stay exactly as they are.

**The recommendation is not to replace fio. It is to stop using fio for performance
measurement and use billet instead.**

---

## The concrete replacement

Four billet Jobs replace the `performance_raid0.py` sweep and the
`stress_test`/`longevity.py` long-running mixed-write jobs:

```
Job 1: ublkpp raid1  — postgresql profile, 30 min, billet.run/1 → s3://nublox-perf/raid1.json
Job 2: ublkpp raid0  — postgresql profile, 30 min, billet.run/1 → s3://nublox-perf/raid0.json
Job 3: md-raid       — postgresql profile, 30 min, billet.run/1 → s3://nublox-perf/md-raid.json
Job 4: raw NVMe      — postgresql profile, 30 min, billet.run/1 → s3://nublox-perf/nvme.json  (--label baseline)
```

```bash
# After all four Jobs complete:
python3 tools/compare.py \
    --baseline s3://nublox-perf/nvme.json \
    s3://nublox-perf/raid1.json \
    s3://nublox-perf/raid0.json \
    s3://nublox-perf/md-raid.json \
    > report.html
```

Total wall time: ~2 hours for four sequential 30-minute runs (the PostgreSQL profile
is destructive; Jobs must not share the device). Compare this to the 6-hour fio
sweep for a single configuration.

The HTML report answers the questions the sweep cannot:
- What is the p99 WAL-commit latency on raid1 vs raw NVMe?
- Does raid0 help or hurt the checkpoint component?
- Which configuration best preserves the reader experience when writers are active?

---

## Summary

| Dimension | fio (current) | billet |
|---|---|---|
| Access mode | filesystem mount (VFS layer) | raw block device (`O_DIRECT`) |
| Latency model | closed-loop, coordinated omission | open-loop Poisson, `intended_ts_ns` |
| Workload shape | single-operation parameter sweep | application-realistic mixed profile |
| Per-component breakdown | none (aggregate only) | per-component HDR histograms |
| Structured output | pod log text | `billet.run/1` JSON + HTML compare |
| Live telemetry | none | Sherlock via Prometheus annotations |
| Approximate test time | ~6 h per configuration | ~30 min per configuration |
| Data integrity | `verify=crc32c` ✓ (keep) | not implemented |
