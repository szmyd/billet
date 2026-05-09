# Sender engine sanity matrix

billet: function generator and oscilloscope for block devices.

`smoke.sh` sweeps a closed-loop 4K random-read across multiple queue depths
and compares billet against `fio --ioengine=io_uring --direct=1`. Read-only,
so it is safe to point at a device hosting a live filesystem.

If billet's numbers diverge from fio outside the tolerances below on a valid
fixture, the sender I/O path has a timing or submission bug and the profile
reports sit on top of bad data.

## Gate

| Metric | Bound                                              |
|--------|----------------------------------------------------|
| IOPS   | billet may not regress more than 5% below fio      |
| p99    | within +/-10% bidirectional                        |

`smoke.sh` exits non-zero if any row fails either bound.

p99 is the sender-correctness signal: matching tail latency means submit/reap
timing is correct. IOPS is asymmetric on purpose -- billet's hot loop has lower
per-op overhead than fio's instrumented path (fio tracks slat/clat splits,
percentile buckets, optional iologs, etc.), so billet running *faster* than fio
is expected on fast devices and is not a fail. billet running *slower* by more
than 5% indicates missed submissions or a stall.

## Valid fixtures

The gate is meaningful only against a fixture where each I/O actually does
work. With O_DIRECT bypassing the page cache, that means:

- A dedicated unused disk (NVMe, SATA SSD, spinner, anything block-shaped).
- A virtual or layered block device exposing a real backing store.
- A `/dev/zramN` block device (kernel zram, RAM-backed but routed through
  the block layer; not the active swap device).

Read-only is the default, so live-filesystem disks are safe targets if the
gate is the only thing being run.

### Invalid fixtures

A loop device backed by a sparse file (`truncate -s ... && losetup`) is **not
a valid gate fixture**. Sparse-file reads return zeros from the page cache
without touching backing storage, so neither billet nor fio actually does
I/O. The benchmark degenerates to measuring io_uring submit/complete loop
overhead, which is structurally different between the two tools (billet
busy-polls; fio uses `submit_and_wait`). billet will appear to outperform
fio by 30-100% in that regime; the result is meaningless.

A loop device backed by a `dd if=/dev/urandom`-filled file is better but
still serves reads from the kernel page cache after warmup, so most of the
workload is memory-bound rather than I/O-bound.

## Run it

```bash
sudo ./example/smoke.sh --device /dev/<dev>
```

A 10s per-config run takes ~1 minute total over the default QD sweep.

## Knobs

```
--device <path>      Block device to test (required)
--duration <s>       Per-config run length (default: 10)
--qds <list>         Comma-separated queue depths (default: 1,4,16,32,64,128)
--results-dir <dir>  Where to drop per-run JSON / fio output
                     (default: /tmp/billet-smoke-<unix>)
```

Environment overrides:

- `BILLET=path/to/billet` (default: `build/Release/src/cli/billet`)
- `FIO=path/to/fio` (default: looked up on `PATH`)

## Output

```
device:   /dev/nvme0n1
duration: 10s
results:  /tmp/billet-smoke-1715275200

qd    billet iops   fio iops      diops%    billet p99us  fio p99us    dp99%     result
----------------------------------------------------------------------------------------------------
1     ...
```

Each per-config run is captured as a `billet.run/1` JSON and a fio JSON in
the results directory; rerun `jq` against them to drill into specific
percentiles or compare the HDR-encoded payloads.

## Sample results

`/dev/nvme0n1` (consumer NVMe), 10s per-config, 2026-05-09:

```
qd    billet iops   fio iops      diops%     billet p99us  fio p99us    dp99%     result
----------------------------------------------------------------------------------------------------
1     40720.9       38126.5           +6.80% 104           105          0.95      PASS
4     142133.8      134713.7          +5.51% 157           153          2.61      PASS
16    316083.5      297544.3          +6.23% 359           350          2.57      PASS
32    345011.7      309471.2         +11.48% 477           453          5.30      PASS
64    350229.3      322611.3          +8.56% 623           594          4.88      PASS
128   347674.5      334733.7          +3.87% 862           823          4.74      PASS
```

Pattern: billet leads fio on IOPS by 4-12%, p99 tracks within 1-6%. Equivalent
device latency confirms timing correctness; the IOPS gap is fio's per-op
bookkeeping overhead on a single thread, not a billet anomaly. At QD=128 the
gap narrows because device latency dominates the per-op CPU cost in both tools.

## Why no internal fio shell-out?

billet never invokes fio at runtime; this script lives in `example/` because
fio comparison is a one-time correctness gate, not part of normal operation.
See the "No runtime shell-outs" note in the project's coding conventions.
