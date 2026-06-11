#pragma once

#include <algorithm>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

#include <billet/workload/pg_wal_commit.hpp>

namespace billet::workload::profiles::detail {

// One closed-loop session: sequential write + immediate fsync, cycling within
// a fixed region. Mirrors a PostgreSQL session with synchronous_commit=on --
// every commit writes a WAL record and calls fdatasync before returning to the
// application. No reads, no group commit, no rate cap.
template < typename Ctx >
exec::task< void > pgwc_session_loop(Ctx& ctx, pg_wal_commit_config const& cfg, uint64_t region_start,
                                      uint64_t region_size) {
    uint64_t cursor = 0;
    while (!ctx.stopped()) {
        op w{};
        w.kind           = op_kind::write;
        w.component_id   = pgwc_component::commit_write;
        w.offset         = region_start + (cursor % region_size);
        w.len            = cfg.write_size_bytes;
        w.intended_ts_ns = ctx.now_ns();
        cursor += cfg.write_size_bytes;
        (void)co_await ctx.submit_op(w);

        if (ctx.stopped()) { break; }

        op f{};
        f.kind           = op_kind::fsync;
        f.component_id   = pgwc_component::commit_fsync;
        f.intended_ts_ns = ctx.now_ns();
        (void)co_await ctx.submit_op(f);
    }
    co_return;
}

template < typename Ctx >
exec::task< void > pg_wal_commit_run_impl(Ctx& ctx, pg_wal_commit_config const& cfg, uint32_t sessions) {
    if (0 == sessions || 0 == cfg.device_size_bytes) { co_return; }

    // Align partition down to write_size_bytes so region_start = partition * i
    // is always write-size-aligned -- required for O_DIRECT on any block size.
    uint64_t const raw_partition = cfg.device_size_bytes / sessions;
    uint64_t const partition     = (raw_partition / cfg.write_size_bytes) * cfg.write_size_bytes;
    uint64_t const region        = std::min(cfg.region_per_session_bytes, partition);
    if (region < cfg.write_size_bytes) { co_return; }

    exec::async_scope emitters;
    for (uint32_t i = 0; sessions > i; ++i) {
        emitters.spawn(pgwc_session_loop(ctx, cfg, partition * i, region));
    }
    co_await emitters.when_empty(stdexec::just());
    co_return;
}

} // namespace billet::workload::profiles::detail
