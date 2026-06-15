# Sender Engine

billet: function generator and oscilloscope for block devices.

The sender engine is the testbed core. The CLI builds a profile sender, the
engine drives that sender on one `io_uring` worker, and every completed op is
accounted into the same `run_summary` shape the report layer serializes.

```mermaid
flowchart TD
    classDef setup fill:#e8f3ff,stroke:#2563eb,stroke-width:2px,color:#0f172a
    classDef profile fill:#ecfdf5,stroke:#059669,stroke-width:2px,color:#052e16
    classDef ring fill:#fef3c7,stroke:#d97706,stroke-width:2px,color:#451a03
    classDef account fill:#f3e8ff,stroke:#7c3aed,stroke-width:2px,color:#2e1065
    classDef decision fill:#fff7ed,stroke:#ea580c,stroke-width:2px,color:#431407
    classDef output fill:#fee2e2,stroke:#dc2626,stroke-width:2px,color:#450a0a

    start([Run requested]):::setup
    configure["Configured<br/>CLI flags parsed, device probed,<br/>profile builder and component_spec selected"]:::setup
    init["Initialized<br/>O_DIRECT fd open, io_uring ready,<br/>scheduler, buffer pool, HDRs allocated"]:::setup
    spawn["Profile running<br/>sender task spawned in async_scope"]:::profile
    poll["Polling<br/>scheduler.poll_once reaps CQEs<br/>and runs continuations"]:::ring

    want{"Profile transition"}:::decision
    timer["Timer armed<br/>schedule_at emits timeout SQE"]:::profile
    submit["Submit requested<br/>op already has intended_ts_ns"]:::profile
    gate{"QD slot available?"}:::decision
    parked["Parked<br/>sender waits for a completion<br/>to release a QD slot"]:::ring
    inflight["In flight<br/>SQE prepared for read, write,<br/>fsync, or timeout"]:::ring
    complete["Completed<br/>CQE returns through scheduler"]:::ring
    account["Accounted<br/>latency = completion - intended<br/>by_kind and by_component updated"]:::account
    resume["Sender resumed<br/>receiver gets completion value"]:::profile
    stopping{"Deadline reached<br/>and profile drained?"}:::decision
    summary["Summarized<br/>run_summary built from accumulators"]:::output
    json["Serialized<br/>billet.run/1 JSON"]:::output
    done([Run complete]):::output

    start --> configure --> init --> spawn --> poll --> want
    want -- "wait for time" --> timer --> inflight
    want -- "submit device op" --> submit --> gate
    gate -- "no" --> parked
    parked -- "slot released by CQE" --> inflight
    gate -- "yes" --> inflight
    inflight --> complete --> account
    account --> resume --> poll
    poll --> stopping
    stopping -- "no" --> want
    stopping -- "yes" --> summary --> json --> done
```

## Flow

1. The CLI probes the target device, builds `run_config`, chooses the sender
   profile, and passes the profile's `component_spec` table into the engine.
2. `run_with_senders` opens the block device, initializes the ring and
   scheduler, reserves the aligned buffer pool, allocates HDR histograms, and
   creates `workload_ctx`.
3. The profile runs as an `exec::task<void>`. It uses `schedule_at` for timers
   and `submit_op` for device work. The profile, not the engine, decides the
   operation's `intended_ts_ns`.
4. `workload_ctx` enforces the global `--qd` device-op cap before an op reaches
   the SQ ring. Extra sender states park until a CQE releases a slot.
5. On completion, the engine records `completion_ts_ns - intended_ts_ns` with
   `hdr_record_value`, updates per-kind and per-component counters, releases
   the QD slot, and resumes the waiting sender.
6. When the deadline expires and profile tasks drain, the engine returns a
   `run_summary`. The report layer turns that into `billet.run/1`.

Timers and device I/O intentionally share the same scheduler and CQE polling
loop. That keeps open-loop arrivals, WAL fsync deadlines, checkpoint periods,
and I/O completions on one clock path instead of splitting the testbed across
independent event loops.

## Multi-worker

`--workers N` runs the flow above on N threads at once. Each worker is fully
share-nothing on the I/O hot path: its own `O_DIRECT` fd, `io_uring`, sender
scheduler, aligned buffer pool, and accumulator. The only shared sinks are
`live_stats` (atomic) and the `sisl::metrics` group (URCU-sharded), both built
for concurrent updates. After every worker joins, the per-worker accumulators
merge into one `run_summary` via `hdr_add`, so percentiles compose exactly.

Placement comes from `engine/topology`, which reads the device's blk-mq
hardware queues (`/sys/class/block/<disk>/mq/*/cpu_list`) and NUMA layout from
sysfs. `plan_workers` assigns each worker a cpuset:

- `mq` (default via `auto`): worker `i` pins to the cpuset of a distinct,
  NUMA-local-first hardware queue, so submission spreads across the device's
  I/O queues instead of saturating one core's queue. `--workers 0` auto-sizes
  to the number of NUMA-local queues.
- `numa` / `linear` / `none`: fallbacks for stacked or synthetic devices
  (md, dm, loop) that expose no hardware queues.

The workload, not the engine, decides what fans out. Read/write I/O is
replicated per worker (each worker drives the device independently with its own
RNG stream); WAL and commit are singletons hosted on worker 0 only, modeling
one physical WAL serializing every session. `pg_wal_commit`, being a pure
commit stream, always runs single-worker regardless of `--workers`.

Because the replicated emitters are rate-driven, increasing `--workers` offers
*more* aggregate load rather than spreading a fixed load across more cores: with
the open-loop `postgresql` emitters, offered read/write IOPS scales with the
worker count, and with the closed-loop profiles aggregate inflight is
`workers * qd`. A `--workers` sweep is therefore a capacity sweep, not a
fixed-load A/B; to isolate submission placement at constant load, hold
`--workers` fixed and vary `--pin-strategy`.
