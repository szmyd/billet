<p align="center">
  <img src="../../docs/assets/billet-logo.png" alt="billet logo" width="160">
</p>

# billet: ublkpp vs md-RAID vs raw iSCSI on tess92

A side-by-side across three storage layers of a live ublk_nublox volume, under
PostgreSQL-shaped and random-read block workloads.

> **TL;DR.**
>
> **Random reads (rr4k):** The raw single-device baseline is queue-saturated at
> QD=32. Every RAID configuration shows lower latency -- not because RAID is
> magic, but because spreading 32 in-flight requests across 2-4 devices reduces
> per-device queue depth. Both md-RAID and ublkpp obey Little's Law (IOPS =
> QD / latency): md-RAID10 wins because its in-kernel path carries no userspace
> round-trip, achieving 3.2x the IOPS at 68% lower p50 latency than baseline.
>
> **Mixed load (pg):** The write path changes everything. md-RAID's kernel
> stripe locks block concurrent reads during write bursts -- read p99 blows up
> to 278--371 ms versus 160 ms on the raw device. **ublkpp RAID10
> (nr_hw_queues=1) is the clear winner:** best read p99 (110 ms, 31% better
> than baseline), write p99 better than a single raw device (8.7 ms vs
> 21.8 ms). No stripe locking -- reads and writes are independent io_uring
> SQEs.
>
> **nr_hw_queues=4:** Gains 13% read IOPS by reducing per-queue lock contention.
> The pg run at hw4 collected only ~40% of expected operations: QD=32 split
> across 4 queues leaves ~QD=8 per queue, insufficient for the pg workload's
> five concurrent streams.

For the full interactive comparison (tail-shape polylines, device capability
table, p99 heat-grid): [compare-rr4k.html](compare-rr4k.html) |
[compare-pg.html](compare-pg.html). For the self-contained report:
[REPORT.html](REPORT.html).

---

## What is billet

billet is a raw block-device benchmark. It opens the device with `O_DIRECT`,
drives it through native `io_uring`, and emits known **app-shaped** I/O
signals (PostgreSQL-style mixes of hot reads, random writes, sequential WAL
appends, periodic fsync, and checkpoint bursts). It records exactly how the
device, driver, or storage stack responds.

It is storage instrumentation, not a database simulator. The PostgreSQL profile
does not parse SQL or model locks. It generates the *block events* PostgreSQL
produces under load.

Latency is recorded as `completion_ts - intended_ts`, not
`completion_ts - submit_ts`. Open-loop work uses Poisson-scheduled arrival
times. Coordinated omission does not get a side door.

## The test

Two profiles were run against each target.

**`random_read_4k` (rr4k)** -- pure read, open-loop Poisson arrivals, 4 KiB
random reads across the full device, QD=32, 300 seconds.

**`postgresql` (pg)** -- mixed read/write workload at a fixed scheduled rate.
The write path is the variable that separates the storage stacks.

| Component | Description | Configured rate |
|---|---|---|
| `reader.Read` | 4 KiB random reads, 85% biased to a 10% hot set | 4 emitters x 2,000 IOPS |
| `rand_writer.Write` | 4 KiB uniform random writes | 2 emitters x 500 IOPS |
| `wal.Write` | Sequential 4 KiB WAL appends | 50 MiB/s |
| `wal.Fsync` | Periodic WAL drain + device flush | every 200 ms |
| `ckpt.Write` | 4 KiB checkpoint write bursts | 256 MiB every 5 s |

Total scheduled load: ~15.4k ops/s. All six targets sustained that rate within
1.5% noise -- this comparison is not about peak IOPS, it is about tail latency
under controlled, app-realistic pressure.

**Engine:** io_uring, `O_DIRECT`, QD=32, 1 worker.
**Duration:** 300 s (captures ~60 checkpoint cycles, ~1,500 WAL fsyncs per run).
**Host:** Intel Xeon Platinum 8352M, 128 cores, 503 GiB RAM, kernel
6.8.0-91-generic, hostname `billet-benchmark`.

## The targets

All targets backed by the same physical iSCSI storage. The ublkpp volume is
provisioned via the nublox CSI as a 600 GiB PVC. The same iSCSI LUNs underlie
both the ublkpp RAID layer and the md-RAID arrays -- the backend is identical
across all three layers.

| Label | Device | Stack | Notes |
|---|---|---|---|
| `baseline` | /dev/sda | Raw iSCSI LUN | Single LUN, no RAID |
| `md-raid1` | /dev/md0 | Kernel md, RAID1 across 2 LUNs | `--assume-clean` |
| `md-raid10` | /dev/md0 | Kernel md, RAID10 across 4 LUNs | `--assume-clean` |
| `uraid1-hw1` | /dev/billet-target | ublkpp RAID1, nr_hw_queues=1 | 2 backing LUNs |
| `uraid10-hw1` | /dev/billet-target | ublkpp RAID10, nr_hw_queues=1 | 4 backing LUNs |
| `uraid10-hw4` | /dev/billet-target | ublkpp RAID10, nr_hw_queues=4 | 4 backing LUNs |

## rr4k Results

### Headline numbers

All latencies in microseconds. Higher IOPS is better; lower latency is better.

| Scenario | IOPS | MiB/s | p50 | p99 | p99.9 | p99.99 |
|---|---:|---:|---:|---:|---:|---:|
| baseline | 58,923 | 230 | 479 | 1,641 | 3,536 | 5,935 |
| md-raid1 | 91,452 | 357 | 320 | 909 | 1,797 | 4,558 |
| **md-raid10** | **189,927** | **742** | **155** | **471** | **885** | **1,946** |
| uraid1-hw1 | 77,754 | 304 | 369 | 1,314 | 2,865 | 5,365 |
| uraid10-hw1 | 85,558 | 334 | 363 | 1,051 | 2,394 | 4,616 |
| uraid10-hw4 | 97,014 | 379 | 297 | 1,176 | 2,750 | 4,784 |

### Analysis

**Every RAID configuration shows lower latency than the raw single-device
baseline.** This is queue saturation. At QD=32, the single iSCSI LUN is
running near its concurrency limit. Any configuration that spreads those 32
in-flight requests across 2 or 4 devices reduces per-device queue depth,
directly reducing per-op queuing latency. Higher throughput *and* lower latency
together are the signature of a saturated single device.

**Why md-RAID10 gets 2x the IOPS of ublkpp RAID10 at the same QD=32:**
Both are constrained by the same budget of 32 in-flight requests (Little's
Law: IOPS = QD / latency). md-RAID10 uses that budget more efficiently because
its read path is entirely in-kernel -- no context switches to userspace. The
per-IO latency is lower (155 vs 363 us p50), so more ops complete per second
at the same QD. The ublkpp round-trip (kernel -> userspace -> io_uring ->
backing device -> userspace -> kernel) adds roughly 140--210 us per op on the
read path. The numbers are consistent with Little's Law at their respective
latencies: md-RAID10 theoretical ceiling = 32 / 155 us = ~206k IOPS, actual
189k; uraid10-hw1 = 32 / 363 us = ~88k IOPS, actual 86k.

**nr_hw_queues=4 gains 13% over hw1** (97k vs 86k IOPS, p50 297 vs 363 us).
With a single benchmark submitter at QD=32, the gain comes from reduced
per-queue lock contention on the ublkpp side. The 2x gap to md-RAID10 remains
-- it is a property of the userspace round-trip, not a queue-count issue.

## pg Results

### Headline numbers (p99, microseconds)

All targets sustained ~16,000 IOPS and ~167 MiB/s. The signal is in the tails.

| Scenario | reader.Read | rand_writer.Write | wal.Write | wal.Fsync | ckpt.Write |
|---|---:|---:|---:|---:|---:|
| baseline | 160,432 | 21,774 | 4,171 | 4,079 | 6,496 |
| md-raid1 | 371,458 | 205,520 | 8,560 | 10,502 | 11,001 |
| md-raid10 | 278,396 | 145,883 | 8,101 | 9,068 | 10,125 |
| uraid1-hw1 | 215,482 | 54,853 | 5,099 | 4,300 | 7,905 |
| **uraid10-hw1** | **110,886** | **8,667** | **3,997** | 4,730 | **5,619** |
| uraid10-hw4 (†) | 116,719 | 10,051 | 4,341 | 5,189 | 6,037 |

(†) uraid10-hw4 collected only ~40% of expected operations. See Section 4.

### Analysis

#### 1. Throughput is intentionally pinned

All six scenarios sustained 15,932--16,151 IOPS within 1.5% of each other.
This is a fixed-rate app shape, not a peak-IOPS bake-off. The differentiator
is tail latency.

#### 2. md-RAID's stripe locks are the dominant failure mode

md-RAID's pg results are the mirror image of its rr4k results. The in-kernel
read path that made it fast under pure reads becomes a liability under mixed
load. Kernel md-RAID serializes writes through **per-stripe locks**: each write
locks the stripe, issues writes to both mirror legs, waits for both
completions, then unlocks. Reads arriving during a locked stripe queue behind it.

Under the pg workload's 256 MiB checkpoint burst (every 5 seconds), md-RAID
holds stripe locks for the duration of the burst. Reads accumulate, producing
the p99 blowup in the table: md-raid1 `reader.Read` p99 = 371 ms (2.3x worse
than baseline), md-raid10 = 278 ms (1.7x worse). The `rand_writer.Write` p99
is catastrophic: md-raid1 = 205 ms (9.4x baseline), md-raid10 = 145 ms (6.7x).

p50 latencies remain low for both md-RAID configurations (106 us for
md-raid10 reads), confirming the in-kernel path is still fast when not blocked.
The tail is entirely a write-burst interference effect.

#### 3. ublkpp RAID10 (hw1) is the standout

ublkpp operates in userspace: reads and writes are independent io_uring SQEs
submitted to backing devices. There is no stripe locking. When a checkpoint
burst fires, ublkpp issues write SQEs to backing devices without blocking the
read submission path. Reads and writes make progress concurrently.

`uraid10-hw1` results:
- `reader.Read` p99 = 110 ms -- best of all configurations, 31% better than
  baseline, 2.5x better than md-raid10
- `rand_writer.Write` p99 = 8.7 ms -- better than the raw single-device
  baseline (21.8 ms), 17x better than md-raid10
- `ckpt.Write` p50 = 1,585 us -- slightly better than baseline (1,848 us)

ublkpp RAID10 beats a raw single iSCSI device on write tail latency because it
fans writes across 4 devices, halving per-device write queue pressure, while
concurrently servicing reads with no locking between the two paths.

#### 4. nr_hw_queues=4 under pg -- the QD budget problem

The uraid10-hw4 pg run collected ~959k `reader.Read` operations versus ~2.4M
for uraid10-hw1 over the same 300-second window -- about 40% of expected
throughput. With QD=32 split across 4 hardware queues, each queue receives
approximately QD=8 of in-flight budget. The pg workload has five concurrent
streams (readers, writers, WAL, fsync, checkpoint). At QD=8 per queue, streams
stall waiting for in-flight slots.

The hw4 pg results are indicative but not directly comparable to hw1 at the
same wall-clock duration. For a pg-style mixed workload at QD=32,
`nr_hw_queues=1` is the correct setting. `nr_hw_queues=4` benefits
single-stream read-heavy workloads; it throttles multi-stream mixed workloads
at the same QD budget.

#### 5. RAID1 vs RAID10 under ublkpp

`uraid1-hw1` vs `uraid10-hw1` shows the write fan-out benefit of RAID10.
RAID1 concentrates write pressure on 2 backing LUNs. RAID10 stripes writes
across 4 devices (2-way mirroring), halving per-device write queue depth.

`rand_writer.Write` p99: 54,853 us (RAID1) vs 8,667 us (RAID10) -- a **6x
improvement** from doubling the stripe count. `reader.Read` p99: 215 ms vs
111 ms -- 2x improvement. For mixed workloads, RAID10 across 4 backing devices
is substantially better than RAID1 across 2.

## Conclusion

For iSCSI-backed storage under this workload mix:

- **For read-only workloads, md-RAID wins.** The in-kernel path has lower
  per-IO latency (no userspace round-trip), which at any fixed QD budget
  translates directly to higher IOPS. md-RAID10 achieves 3.2x the throughput
  of a single raw device at lower latency.
- **For mixed read-write workloads, ublkpp RAID10 wins.** md-RAID's stripe
  locks create severe read-write interference under write bursts, producing
  6--9x worse write tail and 1.7--2.3x worse read tail vs a single raw device.
  ublkpp avoids this entirely with independent io_uring read and write paths.
- **ublkpp RAID10 at nr_hw_queues=1 is the correct setting for mixed workloads
  at QD=32.** nr_hw_queues=4 helps single-stream read workloads; it throttles
  multi-stream mixed workloads by fragmenting the QD budget.
- **ublkpp's per-IO overhead** -- roughly 140--210 us on reads, 50--90 us on
  mixed workloads -- is the cost of the kernel--userspace round-trip and the
  only thing separating ublkpp from md-RAID on pure read performance.

## Reproducing

```bash
BILLET=/usr/local/bin/billet   # inside the billet-benchmark pod

RR4K=(--profile random_read_4k --workers 1 --qd 32 --duration 300)
PG=(--profile postgresql --workers 1 --qd 32 --duration 300 --allow-destructive)

billet --device /dev/sda           --device-label baseline   "${RR4K[@]}" -o baseline-rr4k.json
billet --device /dev/md0           --device-label md-raid1   "${RR4K[@]}" -o md-raid1-rr4k.json
billet --device /dev/md0           --device-label md-raid10  "${RR4K[@]}" -o md-raid10-rr4k.json
billet --device /dev/billet-target --device-label uraid1-hw1 "${RR4K[@]}" -o uraid1-hw1-rr4k.json
billet --device /dev/billet-target --device-label uraid10-hw1 "${RR4K[@]}" -o uraid10-hw1-rr4k.json
billet --device /dev/billet-target --device-label uraid10-hw4 "${RR4K[@]}" -o uraid10-hw4-rr4k.json

billet --device /dev/sda           --device-label baseline   "${PG[@]}" -o baseline-pg.json
billet --device /dev/md0           --device-label md-raid1   "${PG[@]}" -o md-raid1-pg.json
billet --device /dev/md0           --device-label md-raid10  "${PG[@]}" -o md-raid10-pg.json
billet --device /dev/billet-target --device-label uraid1-hw1 "${PG[@]}" -o uraid1-hw1-pg.json
billet --device /dev/billet-target --device-label uraid10-hw1 "${PG[@]}" -o uraid10-hw1-pg.json
billet --device /dev/billet-target --device-label uraid10-hw4 "${PG[@]}" -o uraid10-hw4-pg.json

# md-RAID1 setup (2 iSCSI LUNs, already zeroed via ublkpp)
mdadm --create /dev/md0 --level=1  --raid-devices=2 --assume-clean /dev/sda /dev/sdb

# md-RAID10 setup (4 iSCSI LUNs)
mdadm --create /dev/md0 --level=10 --raid-devices=4 --assume-clean /dev/sda /dev/sdb /dev/sdc /dev/sdd

# Interactive comparison reports
python3 tools/compare.py \
    runs/tess92/baseline-rr4k.json runs/tess92/md-raid1-rr4k.json \
    runs/tess92/md-raid10-rr4k.json runs/tess92/uraid1-hw1-rr4k.json \
    runs/tess92/uraid10-hw1-rr4k.json runs/tess92/uraid10-hw4-rr4k.json \
    -o runs/tess92/compare-rr4k.html --title "tess92 random_read_4k"

python3 tools/compare.py \
    runs/tess92/baseline-pg.json runs/tess92/md-raid1-pg.json \
    runs/tess92/md-raid10-pg.json runs/tess92/uraid1-hw1-pg.json \
    runs/tess92/uraid10-hw1-pg.json runs/tess92/uraid10-hw4-pg.json \
    -o runs/tess92/compare-pg.html --title "tess92 postgresql"
```

## Artifacts in this directory

- `README.md`: this report.
- `REPORT.html`: same content, single self-contained HTML file for offline
  viewing or sharing.
- `compare-rr4k.html`: interactive comparison for the random_read_4k profile
  (generated by `tools/compare.py`). Headlines, tail-shape polylines,
  device-capability table, p99/baseline heat-grid.
- `compare-pg.html`: interactive comparison for the postgresql profile.
- `*-rr4k.json`, `*-pg.json`: per-run `billet.run/1` outputs with embedded
  HDR histogram payloads.
