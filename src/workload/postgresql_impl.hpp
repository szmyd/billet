#pragma once

// Templated implementation of postgresql_run + emitter helpers. Included
// by src/workload/postgresql.cpp (which explicitly instantiates against
// engine::workload_ctx) and by tests that wish to instantiate against an
// alternative ctx (e.g. a manual_scheduler-backed test_ctx). External
// profile authors should NOT include this header -- it is private to
// billet's own profile implementations.

#include <algorithm>
#include <limits>
#include <random>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

#include <billet/workload/postgresql.hpp>
#include <workload/scheduling.hpp>

namespace billet::workload::profiles::detail {

// One open-loop reader: hot-set-biased 8K reads at base Poisson rate.
// Sequential `co_await`: at most one in-flight per coroutine. Profile fans
// out N of these to multiply throughput while keeping each emitter's
// arrival sequence valid.
template < typename Ctx >
exec::task< void > pg_reader_emitter(Ctx& ctx, uint64_t device_size_bytes, uint32_t block_size, uint32_t target_iops,
                                     double hot_set_frac, double locality, uint64_t seed) {
    uint64_t const total_blocks = device_size_bytes / block_size;
    if (0 == total_blocks) { co_return; }

    poisson_emitter             emitter(target_iops, seed);
    std::mt19937_64             rng(make_rng(seed ^ 0xa1a1a1a1ULL));
    std::bernoulli_distribution bernoulli(locality);
    // Clamp hot_blocks to [1, total_blocks]. With hot_set_frac >= 1.0 the
    // hot region covers the whole device; we then pick from the hot dist
    // unconditionally because the cold range below would be empty.
    uint64_t const hot_blocks =
        std::min(total_blocks,
                  std::max< uint64_t >(1, static_cast< uint64_t >(static_cast< double >(total_blocks) * hot_set_frac)));
    bool const     have_cold_range = (hot_blocks < total_blocks);
    uint64_t const cold_lo         = hot_blocks;
    uint64_t const cold_hi         = have_cold_range ? total_blocks - 1 : cold_lo;
    std::uniform_int_distribution< uint64_t > hot_dist(0, hot_blocks - 1);
    std::uniform_int_distribution< uint64_t > cold_dist(cold_lo, cold_hi);

    while (!ctx.stopped()) {
        uint64_t scheduled = 0;
        if (!emitter.try_advance(ctx.now_ns(), scheduled)) {
            co_await ctx.scheduler().schedule_at(emitter.deadline_ns());
            continue;
        }
        // Bernoulli flip is gated on actually having a cold range; with
        // hot_set_frac >= 1.0 there is no cold partition and every read
        // comes from the hot dist unconditionally.
        uint64_t const block_idx = (have_cold_range && !bernoulli(rng)) ? cold_dist(rng) : hot_dist(rng);
        op             o{};
        o.kind           = op_kind::read;
        o.component_id   = pg_component::reader;
        o.offset         = block_idx * block_size;
        o.len            = block_size;
        o.intended_ts_ns = scheduled;
        (void)co_await ctx.submit_op(o);
    }
    co_return;
}

// Open-loop random writer: 8K writes uniformly across the device.
template < typename Ctx >
exec::task< void > pg_rand_writer_emitter(Ctx& ctx, uint64_t device_size_bytes, uint32_t block_size,
                                          uint32_t target_iops, uint64_t seed) {
    uint64_t const total_blocks = device_size_bytes / block_size;
    if (0 == total_blocks) { co_return; }

    poisson_emitter                           emitter(target_iops, seed);
    std::mt19937_64                           rng(make_rng(seed ^ 0xb2b2b2b2ULL));
    std::uniform_int_distribution< uint64_t > dist(0, total_blocks - 1);

    while (!ctx.stopped()) {
        uint64_t scheduled = 0;
        if (!emitter.try_advance(ctx.now_ns(), scheduled)) {
            co_await ctx.scheduler().schedule_at(emitter.deadline_ns());
            continue;
        }
        op o{};
        o.kind           = op_kind::write;
        o.component_id   = pg_component::rand_writer;
        o.offset         = dist(rng) * block_size;
        o.len            = block_size;
        o.intended_ts_ns = scheduled;
        (void)co_await ctx.submit_op(o);
    }
    co_return;
}

// Helper coroutine that spawns submit_op into a scope. Used by pg_wal to
// fire writes without blocking the emission loop on per-op completion.
template < typename Ctx >
exec::task< void > submit_and_forget(Ctx& ctx, op o) {
    (void)co_await ctx.submit_op(o);
    co_return;
}

// Sequential WAL appends with a periodic drain-and-flush cycle. NOT a
// group-commit simulator -- there is no LSN, no commit waiter, no per-
// fsync batching of arrived transactions; see docs/profiles/postgresql.md
// "Limitations vs real Postgres WAL" for the boundaries of this model.
//
// Every write is spawned into `writes_scope` so emission isn't blocked by
// completion. When fsync becomes due, the emitter awaits
// `writes_scope.when_empty()` to drain prior writes, then `co_await`s the
// Fsync with intended_ts_ns set to the moment fsync became due (so the
// engine-computed latency includes the drain interval).
template < typename Ctx >
exec::task< void > pg_wal_emitter(Ctx& ctx, uint64_t region_offset, uint64_t region_size, uint64_t bytes_per_sec,
                                  uint32_t fsync_every_ms, uint32_t fsync_every_writes, uint32_t block_size,
                                  uint64_t seed) {
    uint64_t const  effective_region = (0 < region_size) ? region_size : block_size;
    poisson_emitter emitter(static_cast< double >(bytes_per_sec) / std::max< uint32_t >(1, block_size), seed);
    uint64_t        cursor              = 0;
    uint64_t        last_fsync_ns       = ctx.now_ns();
    uint64_t        last_write_intended = 0;
    uint64_t        writes_issued       = 0;

    exec::async_scope writes_scope;

    while (!ctx.stopped()) {
        // Emission phase: fire writes until fsync becomes due.
        while (true) {
            bool const time_due =
                (0 < fsync_every_ms) &&
                (ctx.now_ns() >= last_fsync_ns + uint64_t(fsync_every_ms) * 1'000'000ULL);
            bool const count_due = (0 < fsync_every_writes) && (writes_issued >= fsync_every_writes);
            if ((time_due || count_due) && 0 < writes_issued) {
                uint64_t const time_due_ns =
                    time_due ? last_fsync_ns + uint64_t(fsync_every_ms) * 1'000'000ULL
                             : std::numeric_limits< uint64_t >::max();
                uint64_t const count_due_ns =
                    count_due ? last_write_intended : std::numeric_limits< uint64_t >::max();
                uint64_t const fsync_due_ns = std::min(time_due_ns, count_due_ns);

                // Drain prior writes so wal.Fsync latency captures the
                // queue-drain cost (commit semantics at the workload
                // layer; the block-layer fsync itself is just a flush).
                co_await writes_scope.when_empty(stdexec::just());

                op fs{};
                fs.kind           = op_kind::fsync;
                fs.component_id   = pg_component::wal;
                fs.intended_ts_ns = fsync_due_ns;
                auto compl_         = co_await ctx.submit_op(fs);
                last_fsync_ns       = fs.intended_ts_ns + static_cast< uint64_t >(compl_.latency_ns);
                writes_issued       = 0;
                last_write_intended = 0;
                break;
            }

            if (ctx.stopped()) { break; }

            uint64_t scheduled = 0;
            if (!emitter.try_advance(ctx.now_ns(), scheduled)) {
                co_await ctx.scheduler().schedule_at(emitter.deadline_ns());
                continue;
            }
            op o{};
            o.kind           = op_kind::write;
            o.component_id   = pg_component::wal;
            o.offset         = region_offset + (cursor % effective_region);
            o.len            = block_size;
            o.intended_ts_ns = scheduled;
            cursor += block_size;
            ++writes_issued;
            last_write_intended = scheduled;
            writes_scope.spawn(submit_and_forget(ctx, o));
        }
    }

    // Final drain on stop: let any spawned writes finish before returning.
    co_await writes_scope.when_empty(stdexec::just());
    co_return;
}

// Periodic burst: idles between periods, then spawns `qd` parallel write
// sub-tasks during a burst until burst_bytes have been issued. Closed-loop
// inside a burst; sub-tasks drain via scope.when_empty before the next
// burst begins.
template < typename Ctx >
exec::task< void > pg_checkpointer_emitter(Ctx& ctx, uint64_t device_size_bytes, uint32_t period_ms,
                                           uint64_t burst_bytes, uint32_t block_size, uint64_t seed, uint32_t qd) {
    uint64_t const total_blocks = device_size_bytes / block_size;
    if (0 == total_blocks) { co_return; }

    // Convert to a block count so the per-burst total is exact regardless
    // of how we shard across sub-tasks. Without this, sub-task slicing in
    // bytes + per-task block rounding could over-emit by up to qd blocks
    // per burst.
    uint64_t const burst_blocks = burst_bytes / block_size;
    if (0 == burst_blocks) { co_return; }
    uint64_t const period_ns = uint64_t(period_ms) * 1'000'000ULL;

    while (!ctx.stopped()) {
        uint64_t const wake_ns = ctx.now_ns() + period_ns;
        co_await ctx.scheduler().schedule_at(wake_ns);
        if (ctx.stopped()) { break; }

        // Distribute `burst_blocks` exactly across `qd` sub-tasks: first
        // `remainder` get one extra block.
        uint64_t const blocks_per_sub = burst_blocks / qd;
        uint64_t const remainder      = burst_blocks % qd;

        exec::async_scope burst_scope;
        for (uint32_t i = 0; qd > i; ++i) {
            uint64_t const child_seed = (0 == seed) ? 0ULL : (seed ^ (uint64_t{i} * 0x9E3779B97F4A7C15ULL));
            uint64_t const my_blocks  = blocks_per_sub + ((i < remainder) ? 1 : 0);
            if (0 == my_blocks) { continue; }
            burst_scope.spawn(
                [](Ctx& ctx_inner, uint32_t bsz, uint64_t blocks_to_emit, uint64_t total_blks,
                   uint64_t inner_seed) -> exec::task< void > {
                    std::mt19937_64                           local_rng = make_rng(inner_seed);
                    std::uniform_int_distribution< uint64_t > local_dist(0, total_blks - 1);
                    while (0 < blocks_to_emit && !ctx_inner.stopped()) {
                        op o{};
                        o.kind           = op_kind::write;
                        o.component_id   = pg_component::ckpt;
                        o.offset         = local_dist(local_rng) * bsz;
                        o.len            = bsz;
                        o.intended_ts_ns = ctx_inner.now_ns();
                        (void)co_await ctx_inner.submit_op(o);
                        --blocks_to_emit;
                    }
                    co_return;
                }(ctx, block_size, my_blocks, total_blocks, child_seed));
        }
        co_await burst_scope.when_empty(stdexec::just());
    }
    co_return;
}

template < typename Ctx >
exec::task< void > postgresql_run_impl(Ctx& ctx, postgresql_config const& cfg, uint32_t qd) {
    exec::async_scope emitters;

    // Skip emitter classes whose target IOPS is 0. poisson_emitter clamps
    // the rate to >= 1.0, so spawning a reader/writer with target_iops=0
    // would silently emit at 1 op/sec instead of being a no-op as the
    // user asked.
    if (0 < cfg.reader_target_iops) {
        for (uint32_t i = 0; cfg.readers > i; ++i) {
            uint64_t const reader_seed =
                (0 == cfg.rng_seed) ? 0ULL : (cfg.rng_seed ^ (uint64_t{0xa1} << 32) ^ uint64_t{i});
            emitters.spawn(pg_reader_emitter(ctx, cfg.device_size_bytes, cfg.block_size_8k, cfg.reader_target_iops,
                                              cfg.hot_set_frac, cfg.locality, reader_seed));
        }
    }
    if (0 < cfg.writer_target_iops) {
        for (uint32_t i = 0; cfg.writers > i; ++i) {
            uint64_t const writer_seed =
                (0 == cfg.rng_seed) ? 0ULL : (cfg.rng_seed ^ (uint64_t{0xb2} << 32) ^ uint64_t{i});
            emitters.spawn(pg_rand_writer_emitter(ctx, cfg.device_size_bytes, cfg.block_size_8k,
                                                   cfg.writer_target_iops, writer_seed));
        }
    }
    if (0 < cfg.wal_bytes_per_sec) {
        emitters.spawn(pg_wal_emitter(ctx, cfg.wal_region_offset, cfg.wal_region_size, cfg.wal_bytes_per_sec,
                                       cfg.wal_fsync_every_ms, cfg.wal_fsync_every_writes, cfg.block_size_8k,
                                       cfg.rng_seed));
    }
    if (0 < cfg.ckpt_period_ms && 0 < cfg.ckpt_burst_bytes) {
        emitters.spawn(pg_checkpointer_emitter(ctx, cfg.device_size_bytes, cfg.ckpt_period_ms, cfg.ckpt_burst_bytes,
                                                cfg.ckpt_block_size, cfg.rng_seed, qd));
    }

    co_await emitters.when_empty(stdexec::just());
    co_return;
}

} // namespace billet::workload::profiles::detail
