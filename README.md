<p align="center">
  <img src="docs/assets/billet-logo.png" alt="billet logo" width="180">
</p>

<h1 align="center">billet</h1>

<p align="center">
  <strong>Function generator and oscilloscope for block devices.</strong>
</p>

<p align="center">
  <a href="LICENSE"><img alt="License: Apache 2.0" src="https://img.shields.io/badge/license-Apache%202.0-BA3937"></a>
  <img alt="C++23" src="https://img.shields.io/badge/C%2B%2B-23-191918">
  <img alt="engine: sender io_uring" src="https://img.shields.io/badge/engine-sender__io__uring-63615B">
</p>

`billet` is a raw block-device benchmark that goes straight to the iron.
No filesystem above to lie to you. No page cache to flatter the numbers.
No fio jobfile soup to misread. Just `O_DIRECT`, native `io_uring`, and
profile coroutines written in C++ that a database engineer can audit.

## What billet Is

billet is storage instrumentation, not a database simulator.

The tagline is literal: billet injects a known app-shaped I/O signal, then
records how the device, driver, or storage backend responds. The PostgreSQL
profile does not try to become PostgreSQL. It emits a controlled mix of
PostgreSQL-shaped block events: hot reads, random data-file writes, sequential
WAL append, periodic WAL drain/flush, and checkpoint bursts.

That makes billet useful for A/B testing storage stacks before asking a
database team to run the real system. If a candidate driver turns a known input
signal into multi-second read p99 or backed-up WAL writes, the driver needs
work. If it preserves the shape, it earns a deeper test with the real database.

## The Score

You want to know what the device does under your app's I/O shape. Not only
synthetic 4K random reads, but your shape: PostgreSQL-style WAL appends,
WAL flushes, checkpoint bursts, hot-set reads, and random data-file writes.

Profiles in billet are sender tasks. A profile gets a `workload_ctx`, waits
on timers through the same `io_uring` scheduler as I/O, submits block ops
with `co_await ctx.submit_op(op)`, and lets the engine account completions
into HDR histograms.

The latency rule is deliberately boring:

```text
latency = completion_ts_ns - intended_ts_ns
```

Closed-loop work sets `intended_ts_ns` at issue time. Open-loop work sets it
to the scheduled arrival time. If an op sat behind earlier work, that wait is
part of the number. Coordinated omission does not get a side door.

Each run emits `billet.run/1`: host metadata, device geometry, profile
parameters, aggregate totals, per-op-kind stats, and per-component/op-kind
stats. HDR payloads are embedded as gzip+base64 so a later tool can re-merge
runs without throwing percentile precision away.

## Current Engine Contract

The sender engine is the only engine; the legacy callback path retired in
Phase 4. Current behavior:

- `--workers N` runs N pinned worker threads (default 1). `--workers 0`
  auto-sizes to one worker per NUMA-local hardware queue discovered on the
  device. Each worker owns its own `io_uring`, aligned buffer pool, and sender
  scheduler.
- `--pin-strategy auto|mq|numa|linear|none` chooses how workers map to CPUs.
  `mq` pins each worker to a distinct blk-mq hardware-queue cpuset (discovered
  from `/sys/class/block/<disk>/mq/*/cpu_list`) so submission spreads across the
  device's I/O queues; `auto` prefers `mq`, then `numa`, then `linear`. Run
  `--probe <dev>` to print the discovered queue/cpuset/NUMA map.
- `--qd` is the per-worker device-op cap enforced inside `workload_ctx`; the
  aggregate inflight ceiling is `workers * qd`.
- Read/write I/O fans out across workers, but WAL and commit stay one
  serialized stream (a single physical WAL) hosted on worker 0: `postgresql`'s
  WAL writer and checkpointer are never replicated, and `pg_wal_commit` (a pure
  commit stream) always runs single-worker.

`--metrics-port <N>` exposes a Prometheus `/metrics` endpoint; the
docker-compose stack in [example/grafana/](example/grafana/) brings up
Prometheus + Grafana with a provisioned billet dashboard.

## Why not just wrap fio?

fio is the gold standard for "turn this knob, sweep that knob, give me
numbers." billet still compares its random-read path against fio in
[example/](example/) because fio is a good external sanity check.

But PostgreSQL performance questions are not just "run randread at QD 32."
They are "what happens to read p99 while checkpoint writes are flushing?" and
"how much of commit cost is WAL drain versus device flush?" fio can be driven
hard, but its job model is not billet's component model, and its output is not
`billet.run/1`.

The sender engine exists so the workload and the accounting live in the same
place: the profile decides when an app-shaped event should have arrived, and
the engine records exactly what the block layer did with it.

## Quick Start

### Prerequisites

- Linux with `io_uring` (5.6+).
- Conan 2.0+, CMake 3.22+, a C++23 compiler.

### Build

```bash
git clone https://github.com/szmyd/billet
cd billet
conan install . -s build_type=Release --build=missing
cmake --preset conan-release
cmake --build build/Release
build/Release/src/cli/billet --version
```

### Probe

```bash
build/Release/src/cli/billet --probe /dev/nvme0n1
```

Reports size, logical/physical block, max I/O, rotational, advertised
discard/FUA/write-zeroes. Read-only.

### Read-only random read

```bash
build/Release/src/cli/billet \
  --device /dev/nvme0n1 \
  --profile random_read_4k \
  --workers 1 --qd 32 --duration 30 \
  --output rr.json
```

Closed-loop 4K random reads. `O_RDONLY` at the fd level. Safe on a device
hosting a live filesystem. Profile details:
[docs/profiles/random_read_4k.md](docs/profiles/random_read_4k.md).

### Postgres-shaped run

Destructive. Will write to the device. Use a disposable target.

```bash
build/Release/src/cli/billet \
  --device /dev/nvme1n1 \
  --profile postgresql \
  --workers 1 --qd 32 --duration 60 \
  --allow-destructive \
  --output pg.json
```

Without `--allow-destructive` you get an interactive `yes` prompt on a TTY.
No flag and no TTY means the run is refused.

PostgreSQL knobs:

```text
--pg-readers N             Reader emitter count
--pg-reader-iops N         Per-reader target IOPS (open-loop Poisson)
--pg-writers N             Random-writer emitter count
--pg-writer-iops N         Per-writer target IOPS
--pg-wal-mb-per-sec N      WAL append target throughput, MiB/s
--pg-wal-fsync-ms N        Periodic Fsync interval, ms (0 disables)
--pg-ckpt-period-ms N      Checkpoint burst period, ms
--pg-ckpt-burst-mb N       Bytes per checkpoint burst, MiB
--pg-hot-set-frac F        Fraction of the device that's the read hot set
--pg-locality F            Probability a read targets the hot set
```

Profile details: [docs/profiles/postgresql.md](docs/profiles/postgresql.md).

## Latency Accounting

The engine records the latency it is handed by the sender model:

```text
completion_ts_ns - intended_ts_ns
```

Then it calls `hdr_record_value`. It does not call
`hdr_record_corrected_value`; open-loop senders already put the scheduled
arrival timestamp in the op. Correcting again would synthesize phantom samples
on top of a value that already includes queueing delay.

For PostgreSQL WAL fsyncs, `intended_ts_ns` is the instant the fsync became
due, before the WAL write drain. The resulting `wal.Fsync` latency is
"drain plus flush", which is the drain-and-flush *component* of commit
cost. The current profile does not model group-commit batching; see
[docs/profiles/postgresql.md](docs/profiles/postgresql.md#limitations-vs-real-postgres-wal)
for what the WAL model is and isn't.

## The Schema

```json
{
  "schema_version": "billet.run/1",
  "run_id": "<26-char ULID>",
  "started_at": "<ISO 8601 UTC>",
  "duration_s": 60.0,
  "host": {
    "hostname": "...",
    "kernel": "...",
    "cpu_model": "...",
    "cores": 16,
    "ram_gb": 64.0
  },
  "device": {
    "path": "/dev/nvme1n1",
    "size_bytes": 1000204886016,
    "logical_block": 512,
    "physical_block": 4096,
    "max_io_kb": 1024,
    "rotational": false,
    "discard_supported": true,
    "fua_supported": true,
    "write_zeroes_supported": true,
    "label": ""
  },
  "profile": {
    "name": "postgresql",
    "version": "1",
    "params": {
      "readers": "4",
      "reader_target_iops": "2000",
      "writers": "2",
      "writer_target_iops": "500",
      "wal_mb_per_sec": "50",
      "wal_fsync_ms": "200",
      "ckpt_period_ms": "5000",
      "ckpt_burst_mb": "256",
      "hot_set_frac": "0.1000",
      "locality": "0.8500",
      "workers": "1"
    }
  },
  "engine": {
    "name": "io_uring",
    "qd_per_worker": 32,
    "workers": 1,
    "sqpoll": false,
    "o_direct": true
  },
  "results": {
    "summary": {
      "ops_total": 123456,
      "bytes_total": 987654321,
      "iops_mean": 2057.6,
      "throughput_mibs": 156.4,
      "errors": 0,
      "component_drops": 0
    },
    "by_op": {
      "Read":  { "count": 100000, "bytes": 819200000, "p99_us": 410, "hdr_b64": "..." },
      "Write": { "count": 23000,  "bytes": 188416000, "p99_us": 950, "hdr_b64": "..." },
      "Fsync": { "count": 300,    "bytes": 0,         "p99_us": 2200, "hdr_b64": "..." }
    },
    "by_phase": {},
    "by_component": {
      "reader.Read":       { "count": 100000, "bytes": 819200000, "p99_us": 410, "hdr_b64": "..." },
      "rand_writer.Write": { "count": 12000,  "bytes": 98304000,  "p99_us": 880, "hdr_b64": "..." },
      "wal.Write":         { "count": 10000,  "bytes": 81920000,  "p99_us": 540, "hdr_b64": "..." },
      "wal.Fsync":         { "count": 300,    "bytes": 0,         "p99_us": 2200, "hdr_b64": "..." },
      "ckpt.Write":        { "count": 1000,   "bytes": 65536000,  "p99_us": 1800, "hdr_b64": "..." }
    }
  }
}
```

`by_op` answers "did reads/writes/fsyncs move?" `by_component` answers "which
part of the profile moved?" For PostgreSQL, `Write` alone is too blunt:
random data-file writes, WAL appends, and checkpoint writes are different
distributions.

`component_drops` should be zero. Non-zero means a profile emitted an op whose
`component_id` and `kind` were not declared in that profile's component table;
then `by_op` remains correct, but `by_component` is missing those ops.

`hdr_b64` is the HDR payload. Re-merge it instead of scraping a screenshot of
one run and pretending the p99 decimal is the whole story.

## Architecture

```text
billet/
├── docs/assets/            README art and project images
├── docs/profiles/          Sender-model workload profile notes
├── include/billet/         Public op/component schema types
├── src/
│   ├── engine/             Sender io_uring engine, workload_ctx, buffers, HDR helpers
│   ├── workload/           Sender profiles and scheduling helpers
│   ├── report/             JSON serialization, host metadata, ULID
│   └── cli/                Command-line wiring and progress reporter
├── test/                   GTest suite
├── example/                fio comparison smoke matrix
└── cmake/                  Sanitizer and coverage helpers
```

The pieces that matter:

- `workload_ctx` is the profile's only way into the engine: timers,
  `submit_op`, deadline checks, and queue-depth backpressure.
- Profiles are `exec::task<void>` pipelines. They decide when app-shaped work
  should arrive and stamp `intended_ts_ns` accordingly.
- The sender engine owns the fd, `io_uring`, aligned buffers, CQE polling, HDR
  accumulation, and component/op accounting.
- Reports consume a completed `run_summary`; they do not peek at live engine
  state.

Engine flow diagram: [docs/engine.md](docs/engine.md).

Profile workflow diagrams:
[random_read_4k](docs/profiles/random_read_4k.md#flow-diagram),
[postgresql](docs/profiles/postgresql.md#workflow-state-diagram).

## Validation

Two layers keep the sender path honest.

**fio smoke matrix.** billet's `random_read_4k` sender run is compared with
fio's `--rw=randread --ioengine=io_uring --direct=1` across queue depths.
billet IOPS may not regress more than 5% below fio; p99 must stay within
+/-10% bidirectional. Sparse-file loop devices are not valid fixtures; use a
real disk or `/dev/zramN`.

```bash
sudo ./example/smoke.sh --device /dev/<dev>
```

**Unit tests.** GTest covers HDR merge, aligned-buffer invariants, op-kind
round-trip, ULID format, ISO 8601, JSON round-trip, profile layouts, sender
profile behavior, and WAL drain semantics.

```bash
ctest --test-dir build/Release --output-on-failure
```

## Code Style

- 4-space indent, 120-column lines, pointer-left (`Type* ptr`).
- C++23, `#pragma once`, east `const`, Yoda comparisons.
- Types `lower_snake_case`. Methods `snake_case`. Members `_snake_case`.
  Constants `k_snake_case`. Enum values scoped lower-snake.

## Deps

```text
sisl              logging, options, async io_uring scheduler
stdexec           sender/task composition
liburing          native io_uring submission
HdrHistogram_c    latency histograms + encoded payloads
nlohmann_json     JSON serialization
indicators        TTY progress display
```

## License

Apache License 2.0. See [LICENSE](LICENSE).

Primary author: [Brian Szmyd](https://github.com/szmyd)
