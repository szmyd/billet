# Candidate Storage Driver For PostgreSQL Workloads

billet: function generator and oscilloscope for block devices.

Audience: PostgreSQL storage/performance reviewers evaluating whether a
candidate block driver or virtual storage backend can replace or augment the
current Linux `md` RAID path.

Status: early performance evaluation. The data below comes from billet
PostgreSQL-shaped block-device runs: 120 seconds, one worker, queue depth 32,
default PG mix of hot reads, random data-file writes, WAL writes/fsyncs, and
checkpoint bursts. Every compared run completed with `errors=0` and
`component_drops=0`.

This is a living product/RCA page. As the driver changes, update the measured
regressions, the current theory, and the mitigation plan here so database teams
see the current state rather than stale benchmark notes.

## Short Version

The candidate driver can sustain the requested PostgreSQL-shaped throughput in
these tests. It does not currently match `md` tail latency.

The headline is not "the driver is slower at steady-state IOPS." The headline
is "the driver occasionally stalls long enough that open-loop PostgreSQL-like
reads and writes report very large tail latency, then catches up." That matters
for PostgreSQL because p99 and above are often the numbers users feel as query
stalls or commit stalls.

Current recommendation: do not position the candidate driver as a drop-in `md`
replacement for latency-sensitive PostgreSQL deployments yet. Treat it as a
promising control plane and experimentation path that needs targeted work on
tail latency and flush semantics before it can make an honest `md` parity
claim.

## What A Candidate Driver Offers

A programmable block driver or virtual storage backend gives us a place to
implement and observe behavior that is hard to change in kernel `md`:

- RAID or placement policy in ordinary project code with a smaller iteration
  loop.
- User-recoverable target lifecycle.
- A natural place for richer per-layer metrics.
- Future features like smarter read routing, live replacement policy, and
  workload-aware behavior.

That flexibility is the product value. The risk is that the extra driver path
adds scheduling, queueing, and context-switch costs that `md` does not pay.

## Current Test Shape

All runs used the same workload knobs:

| Workload component | Rate/shape |
| --- | --- |
| `reader.Read` | 4 readers, 2,000 IOPS each, hot-set biased 8 KiB reads |
| `rand_writer.Write` | 2 writers, 500 IOPS each, random 8 KiB writes |
| `wal.Write` | 50 MiB/s sequential 8 KiB WAL appends |
| `wal.Fsync` | every 200 ms, after draining prior WAL writes |
| `ckpt.Write` | 256 MiB checkpoint burst every 5 seconds, 64 KiB writes |

The useful comparison is component p99 latency, not total throughput alone.

## Throughput Result

Throughput is close across stacks:

| Target | Mean IOPS | Throughput |
| --- | ---: | ---: |
| raw NVMe baseline | 15,978 | 167.1 MiB/s |
| `md0` RAID0 | 15,858 | 166.0 MiB/s |
| candidate RAID0, 1 hardware queue | 15,749 | 164.8 MiB/s |
| candidate RAID0, 2 hardware queues | 15,854 | 165.9 MiB/s |
| `md0` RAID1 | 15,821 | 163.8 MiB/s |
| candidate RAID1, 1 hardware queue | 15,795 | 163.5 MiB/s |
| candidate RAID1, 2 hardware queues | 15,538 | 160.8 MiB/s |
| candidate RAID1, 4 hardware queues | 15,718 | 162.7 MiB/s |

Interpretation: the workload is mostly being delivered. The regression is in
tail behavior, not aggregate completion rate.

## Known Regressions vs md

### RAID0

RAID0 is the clearest regression. The candidate driver keeps up on throughput,
but read p99 becomes multi-second.

| Component p99 | `md0` RAID0 | candidate RAID0, 1 hardware queue | candidate RAID0, 2 hardware queues |
| --- | ---: | ---: | ---: |
| `reader.Read` | 642 us | 4.73 s | 2.78 s |
| `rand_writer.Write` | 1.16 ms | 62.7 ms | 6.6 ms |
| `wal.Write` | 964 us | 6.0 ms | 2.6 ms |
| `wal.Fsync` | 2.6 ms | 3.9 ms | 1.5 ms |
| `ckpt.Write` | 6.4 ms | 15.7 ms | 6.8 ms |

The 2-hardware-queue run improves substantially, but `reader.Read` still
regresses by thousands of times versus `md0` RAID0.

### RAID1

RAID1 is less one-sided because `md0` RAID1 already has large read/write tails
in this workload. The candidate RAID1 configuration with 4 hardware queues is
close to `md0` on most write paths, but 2 hardware queues is clearly worse.

| Component p99 | `md0` RAID1 | candidate RAID1, 1 hardware queue | candidate RAID1, 2 hardware queues | candidate RAID1, 4 hardware queues |
| --- | ---: | ---: | ---: | ---: |
| `reader.Read` | 443 ms | 1.09 s | 4.45 s | 586 ms |
| `rand_writer.Write` | 366 ms | 386 ms | 694 ms | 402 ms |
| `wal.Write` | 6.1 ms | 7.4 ms | 19.2 ms | 6.6 ms |
| `wal.Fsync` | 13.9 ms | 6.7 ms | 16.9 ms | 6.9 ms |
| `ckpt.Write` | 8.7 ms | 9.0 ms | 23.4 ms | 8.8 ms |

The 4-hardware-queue RAID1 result is the most encouraging current datapoint. It
suggests the driver can get close on the write-heavy paths once queueing is
shaped correctly. Reads still need work.

## RCA Snapshot

| Observation | Evidence | Current interpretation | Confidence |
| --- | --- | --- | --- |
| Throughput is near target | All runs complete roughly 15.5K-16K IOPS | The driver is not simply underpowered or failing requests | High |
| Tail latency regresses far more than mean throughput | Multi-second `reader.Read` p99 with normal final IOPS | Requests are waiting in bursts, then catching up | High |
| Hardware queue count changes the result dramatically | RAID1 `hw4` is much better than RAID1 `hw2`; RAID0 `hw2` better than `hw1` | Queue/thread scheduling and driver-side dispatch are part of the issue | High |
| Read tails are worse than WAL write tails | RAID0 candidate read p99 is seconds while WAL write p99 is milliseconds | Foreground reads may be starved behind target work or backing-device work during mixed load | Medium |
| WAL fsync may not be directly comparable | A candidate backend may treat flush as a no-op or write-through completion | `wal.Fsync` may measure WAL drain rather than true device flush durability | High |
| Device geometry differs | `md` exposes 512-byte logical blocks; the candidate exposes 4 KiB logical blocks and different max I/O | Some scheduler/merge behavior may differ before the driver even handles the request | Medium |

## Likely Why

The working theory is driver-side queueing in the layered block path.

With `md`, the kernel block layer handles request dispatch, merging,
scheduling, and RAID fanout internally. With a virtual or layered driver, each
block request may cross an extra target boundary before backend I/O is issued.
The driver path then has to:

- receive block requests,
- run the mapping or placement path,
- submit backend I/O,
- process backend completions,
- run any coroutine or callback continuations,
- complete the original block request back to the kernel.

That architecture is flexible, but the driver queue is now part of the latency
path. Under a mixed PostgreSQL-shaped workload, checkpoint writes and random
writes can create bursts of driver-side work. If foreground reads wait behind
that work, billet records the wait as read latency. Because the workload is
open-loop, those delayed reads can still complete later and preserve aggregate
IOPS while ruining p99.

That matches the data: tiny p50, normal throughput, very large p99.

## What We Can Do About It

### 1. Add per-stage latency instrumentation

Before tuning, split driver latency into stages:

| Stage | Question answered |
| --- | --- |
| kernel dispatch to driver receive | Is the block-to-driver boundary backing up? |
| driver receive to backend submit | Is mapping or target scheduling delaying work? |
| backend submit to backend completion | Is the underlying device slow or saturated? |
| backend completion to block completion | Are completion paths delaying replies? |

This should be exposed as metrics per operation type and workload component.
The goal is to stop guessing where the seconds are spent.

### 2. Run isolation tests

Use the same billet profile, but disable one workload component at a time:

| Test | Purpose |
| --- | --- |
| `random_read_4k` only | Proves whether the bare driver read path is healthy |
| PostgreSQL profile with checkpoints disabled | Tests whether checkpoint bursts cause read starvation |
| PostgreSQL profile with random writers disabled | Tests steady random-write interference |
| PostgreSQL profile with WAL disabled | Tests whether WAL stream/fanout contributes |
| QD sweep: 1, 4, 8, 16, 32, 64 | Finds the queue-depth knee |
| hardware queue sweep | Finds whether more queues help or hurt |

Acceptance for this phase is not "beat md." It is identifying which component
triggers the tail.

### 3. Fix queue/thread placement

The hardware queue sensitivity suggests CPU scheduling matters. We should test:

- pinning driver queue threads away from the billet load generator,
- matching hardware queues to physical cores,
- avoiding shared cores with IRQ-heavy devices,
- testing queue depth per hardware queue instead of only total target queue
  depth.

If pinning collapses the multi-second p99, the regression is mostly dispatch
scheduling rather than mapping logic.

### 4. Improve request fairness

If instrumentation confirms reads wait behind write/checkpoint work in the
driver, the driver needs fairness rules:

- avoid letting checkpoint-sized write bursts monopolize a queue,
- consider separate read/write lanes inside the driver,
- cap per-request fanout work before returning to completion processing,
- evaluate whether the queue loop should drain fewer completions per pass.

This is the product-level fix: preserve the driver's flexibility while making
foreground reads predictable.

### 5. Make WAL flush semantics honest

Some drivers can complete flush immediately because the backend is write-through
or because flush is not implemented yet. That is useful to know, but it means
`wal.Fsync` is not automatically an apples-to-apples durability comparison with
`md`.

Before making PostgreSQL durability claims, the driver should implement a real
flush/FUA path or clearly report that the current target measures drain-only
cost.

### 6. Normalize block geometry

The candidate driver and `md` present different block geometry in these runs.
That may affect the kernel scheduler and merge behavior.

We should either:

- expose geometry closer to the backing devices / `md`, or
- run a controlled matrix where geometry is intentionally held constant.

This is probably not the whole regression, but it is an easy source of noise to
remove.

## What Success Looks Like

For a PostgreSQL-facing `md` replacement claim, a reasonable first bar is:

- Zero errors and zero component drops.
- Mean throughput within 5% of `md` for the same topology.
- No multi-second p99 under the default PostgreSQL-shaped workload.
- `reader.Read`, `wal.Write`, and `ckpt.Write` p99 within 2x `md` for the same
  topology.
- `wal.Fsync` compared only after real flush semantics exist, or labeled as
  drain-only.
- Regression matrix covers RAID0 and RAID1 across queue depth and hardware
  queue counts.

## Current Positioning

The candidate driver is not ready to be sold to the PostgreSQL team as "`md`,
but programmable."

The better positioning today:

> The candidate storage driver gives us control and observability we cannot get
> from `md`. The current prototype sustains the target PostgreSQL-shaped
> throughput, but we have identified tail-latency regressions versus `md`,
> especially on RAID0 reads and some RAID1 queue configurations. The next
> milestone is per-stage instrumentation and fairness work to reduce those
> tails while preserving the flexibility that makes the driver useful.

That is honest and useful. It acknowledges the regression without throwing away
the architecture.

## Questions For The PostgreSQL Team

- Which number is the release gate: read p99, commit p99, checkpoint impact, or
  a combined SLA?
- Is RAID0 relevant to production, or only RAID1/RAID10?
- How much CPU budget can the storage target consume?
- Do they require true flush/FUA semantics for the benchmark, or is drain-only
  useful for an early queueing study?
- What `md` configuration and kernel tunables should be considered the
  reference setup?

## Next Milestone

Produce one follow-up report with:

1. The same `md` vs candidate-driver comparison.
2. Per-stage driver latency breakdown.
3. A component-isolation matrix showing which workload source creates the tail.
4. A recommended target configuration for queue count, queue depth, and CPU
   placement.

If that report shows the multi-second read tails are gone or explained by a
fixable queue placement issue, the candidate driver becomes credible for deeper
PostgreSQL evaluation.
