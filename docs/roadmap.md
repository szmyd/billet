# Roadmap

billet: function generator and oscilloscope for block devices.

Current snapshot after the sender-engine work. The narrative sections below
are open or deferred work only. Completed milestones stay in the status table
so future sessions can see what already landed without re-reading old plans.

> billet already detects unacceptable storage tails and can report them through
> JSON, Grafana, and comparison HTML. The remaining work mostly does one of
> three things: explain where a tail came from, broaden the A/B surface, or make
> profile authoring less bespoke.

## Stack-Boundary Observability

billet measures `intended_ts -> completion_ts`. Anything inside a layered
driver/backend, such as a virtual block target queue, backend submission path,
or backend completion path, is invisible from billet alone. This is the
highest-leverage next area because layered-driver comparisons can show large
tail regressions that need attribution before we scale load.

### Driver/backend per-stage instrumentation

Add per-stage timestamps and per-stage HDRs inside the driver or storage
backend under test: request received, backend submit, backend completion, and
request completion. Aggregate by the same `(component, op_kind)` cell keys
billet uses.

**Why**: turns "reader.Read p99 is multi-second" into "time was spent before
backend submit" or "backend IO itself was slow." This usually lives outside
billet, but it is a dependency for making layered-driver comparisons
actionable.

### Per-op correlation key

Research whether a billet `request_id` can survive through the Linux block
layer, the driver/backend queueing path, and the backend I/O path. If the carry
path exists, extend `workload::op` and the io_uring user_data encoding. If it
does not, document the break point.

**Why**: a stable id lets a merge tool stitch `billet.intended_ts`,
`driver.stage_ts[N]`, and `billet.completion_ts` into one request timeline.

### Stage-attributed Schema Slot

Define a stable place under `results` in `billet.run/1` where an external
driver/backend can attach per-stage HDRs keyed by the existing component/op
cells. Billet does not need to fill this slot itself.

**Why**: the compare tool and product brief should not need one-off parsing for
every backend that learns how to expose stage latency.

### External Stage Report

Add a small report path that combines billet JSON with an external stage file
from the driver/backend under test and emits a single comparison artifact.

**Why**: the Postgres-facing brief should be backed by repeatable evidence, not
manual copy/paste from several tools.

### Flush/FUA Semantics Gate

Make the reports explicit about whether a backend's WAL flush path represents a
real durability operation or only billet's pre-flush drain. This likely needs a
small driver/backend-side signal once flush handling is implemented or
intentionally declared as a no-op.

**Why**: `wal.Fsync` must not be read as "stable on media" when the backend
only measured queue drain. The distinction matters more than the exact p99.

## A/B Workflow

The purpose of billet is to run candidate storage stacks before asking database
teams to test them. The next workflow work should reduce hand-run drift and
make comparisons reproducible.

### Validation Matrix

Expand beyond the current ad hoc runs: baseline block device, md raid0/raid1,
candidate driver raid0/raid1, hardware queue count variants, fixed `--qd`,
fixed duration, and both `random_read_4k` and `postgresql`.

**Why**: a single good or bad run is not enough to decide whether a driver
change is acceptable. The comparison needs a stable matrix that can be rerun
after every candidate optimization.

### Matrix Runner

A k8s-native flavor is shipped (`example/k8s/` + `.jenkins/`):
per-scenario Job manifests, image-baked runner script with fixed
flags, `collect.sh` for `kubectl cp` + PV resolution, and
`compare-collected.sh` to drive `tools/compare.py`. The runner
captures an audit sidecar (job/pod/namespace/node/PVC/PV, kernel,
cpu model, cpu count visible to the pod, start/end epoch) so
cross-cluster runs sort and diff cleanly.

A local-shell variant (for non-k8s comparison runs on a workstation)
would still be useful but is lower priority now that the k8s path is
operational.

**Why**: hand-run shell loops have already produced wrong runs in
this project (the `name=foo sudo billet ...` prefix-assignment
mistake). The k8s flavor eliminates that whole class of error and
adds audit identity that survives outside the cluster.

### fio Triangulation

Keep a small set of fio sanity comparisons for simple shapes: random read and
WAL-like append with periodic flush.

**Why**: fio is not the product, but it is a useful skeptical-reader check that
billet is not inventing impossible numbers.

## Profile Fidelity

### Buffered / Group-Commit WAL Mode

The current `postgresql` profile is a stress-canary model: open-loop WAL append
plus periodic drain-and-flush. That is useful for storage-layer fairness and
queueing diagnosis, but it is not a full Postgres commit model.

Add a second WAL mode that batches transactions into per-fsync groups, tracks
LSN-keyed commit waiters, and reports commit latency as
`commit_request_ts -> fsync_complete_ts`. Keep the current mode, but name it
clearly as open-loop append.

**Why**: database teams will ask whether the number is device pressure or
commit latency. The tool should have a mode for each question.

### MongoDB / WiredTiger Profile Sketch

Draft a second database-shaped profile after the PostgreSQL buffered WAL model
is clear. Start with WiredTiger's journal, checkpoint, and data-file write
patterns rather than trying to clone the whole database.

**Why**: a second profile prevents the sender/profile APIs from becoming
Postgres-shaped by accident and matches the broader A/B goal.

## Engine Scale

### Multi-worker

Landed. `--workers N` fans out N pinned worker threads, each owning its own
`io_uring`, sender scheduler, and aligned buffer pool, feeding the shared
(URCU-sharded) metrics group and a merged `run_summary`. `--workers 0`
auto-sizes to one worker per NUMA-local blk-mq hardware queue, discovered from
sysfs (`engine/topology`). `--pin-strategy auto|mq|numa|linear|none` controls
CPU placement; `mq` pins each worker to a distinct hardware-queue cpuset so
submission spreads across the device's I/O queues instead of bottlenecking one
core. Read/write I/O fans out across workers; WAL and commit stay a single
serialized stream (one physical WAL) on worker 0.

The original sequencing note (defer until stack-boundary observability exists)
was relaxed: multi-worker landed first as a pressure tool. Stage attribution
remains the next lever for explaining where multi-worker tails come from, and
per-worker metric labels (below) are the immediate follow-up.

### Per-worker Metrics Labels

Once multi-worker exists, add a worker dimension to per-cell histograms and
gauges.

**Why**: aggregate p99 can hide one overloaded submitter. Worker labels make
skew visible without changing the run JSON contract.

## Public Surface And Cross-Repo Cleanup

### Awaitable `cqe_state` In sisl

Layer a coroutine-awaitable wrapper on top of the callback `cqe_state`.

**Why**: billet can use the callback primitive directly, but future
coroutine-first driver/backend code would benefit from a small ready-made
wrapper.

### Driver/backend adoption of sisl encoding helpers

After sisl ships the user_data helpers, driver/backend code can replace local
encoding bits with the shared helpers.

**Why**: one CQE user_data contract across sisl, billet, and storage drivers
keeps later correlation work from splitting three ways.

### Engine Header Promotion

Move the sender workload context and public engine-facing types into
`include/billet/engine/`.

**Why**: external profile authors need a supported surface instead of including
private `src/engine` headers.

### Profile-author Guide

Write a short guide covering `op`, `op_kind`, `component_spec`,
`workload_ctx`, sender lifetime, and a small worked profile.

**Why**: once the headers are public, the docs should tell a new author how not
to fight the engine.

### stdexec Conan Recipe

Move stdexec dependency management out of per-repo `FetchContent` once a usable
Conan recipe exists.

**Why**: sisl should own the dependency cleanly so consumers do not each carry
their own fetch block.

## Status Quick-reference

| Item | Status |
| --- | --- |
| postgres profile (stress-canary / open-loop WAL) | done |
| postgres profile docs and flow diagrams | done |
| [PostgreSQL candidate-driver comparison brief](storage-driver-postgres-brief.md) | done, living document |
| WAL drain validated on `/dev/loop0` | done |
| sender engine + qd backpressure | done |
| pg_wal drain-ordering test with parked completions | done |
| live observability wireup | done |
| per-entity dashboard overlay | done |
| `compare.py`: bars + spectrum + heat-grid | done |
| per-profile harness coverage | done |
| sisl PR finalization | done |
| k8s deployment scaffolding (`example/k8s/`) | done |
| Jenkins build pipeline + container image (`.jenkins/`) | done |
| audit metadata sidecar (pod/node/PVC/PV + host facts) | done |
| matrix runner (k8s flavor) | done |
| matrix runner (local-shell flavor) | open, lower priority |
| postgres profile (buffered-commit WAL) | open |
| MongoDB / WiredTiger profile sketch | open, after buffered WAL |
| validation matrix beyond one-off runs | open |
| A/B matrix runner | open |
| fio triangulation checks | open |
| flush/FUA semantics gate | open, driver/backend-dependent |
| driver/backend per-stage instrumentation | open, out-of-tree |
| per-op correlation key | open, design/research |
| stage-attribution schema in `billet.run/1` | open, design |
| external stage report | open |
| multi-worker | done (topology-aware cpuset pinning, WAL/commit single-stream) |
| per-worker metrics labels | open, after multi-worker |
| awaitable `cqe_state` | open |
| driver/backend adoption of sisl encoding helpers | open |
| engine header promotion | open |
| profile-author guide | open |
| stdexec Conan recipe | open, cross-project |
