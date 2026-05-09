#include <gtest/gtest.h>

#include <chrono>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

#include <billet/workload/postgresql.hpp>
#include <workload/postgresql_impl.hpp>

#include "test_workload_ctx.hpp"

namespace {

using billet::test::test_workload_ctx;
using billet::workload::op_kind;
using billet::workload::profiles::detail::pg_wal_emitter;
namespace pg_component = billet::workload::profiles::pg_component;

// First WAL fsync cycle (200 ms at 50 MiB/s, 8 KiB blocks) emits N
// wal.Write ops followed by exactly one wal.Fsync whose intended_ts_ns
// equals the moment fsync became due. This is the property the legacy
// callback path enforced via wal_state + complete_all; with the sender
// engine the same property falls out of the drain pattern in pg_wal_emitter
// (writes_scope.when_empty + intended_ts capture before the await).
TEST(pg_wal, first_cycle_emits_writes_then_fsync_at_due_time) {
    test_workload_ctx ctx;

    constexpr uint64_t k_bytes_per_sec = 50ull << 20;
    constexpr uint32_t k_block_size    = 8192;
    constexpr uint32_t k_fsync_ms      = 200;
    constexpr uint64_t k_fsync_due_ns  = uint64_t(k_fsync_ms) * 1'000'000ULL;

    exec::async_scope scope;
    scope.spawn(pg_wal_emitter(ctx, /*region_offset=*/0, /*region_size=*/1ULL << 30, k_bytes_per_sec, k_fsync_ms,
                               /*fsync_every_writes=*/0, k_block_size, /*seed=*/12345));

    // Drive virtual time to the fsync deadline in 1 ms chunks. Each
    // chunk advances _now to the chunk boundary and drains all timers
    // with earlier deadlines, so the emitter's fsync_due check
    // (now_ns() >= 200 ms) only flips true once the run reaches that
    // boundary -- not on the first emitted write, which it would in a
    // single 200 ms jump.
    ctx.step_chunked(std::chrono::nanoseconds{k_fsync_due_ns}, std::chrono::milliseconds{1});

    // Wind down the emitter: flip stopped() and step a small amount so
    // its currently-pending Poisson timer fires, the emitter observes
    // stopped() at the inner loop top, and unwinds through its final
    // writes_scope drain.
    ctx.stop();
    ctx.scheduler().step(std::chrono::milliseconds{1});

    stdexec::sync_wait(scope.when_empty(stdexec::just()));

    auto const& log = ctx.log();
    ASSERT_GE(log.size(), 2u);

    size_t first_fsync = log.size();
    for (size_t i = 0; log.size() > i; ++i) {
        if (op_kind::fsync == log[i].kind) {
            first_fsync = i;
            break;
        }
    }
    ASSERT_LT(first_fsync, log.size()) << "expected at least one fsync in the first cycle";

    EXPECT_EQ(pg_component::wal, log[first_fsync].component_id);
    EXPECT_EQ(k_fsync_due_ns, log[first_fsync].intended_ts_ns);

    // Every entry before the first fsync must be a wal.Write. The drain
    // discipline guarantees this: writes spawned into writes_scope all
    // record their submit_op call (synchronous in the test ctx) before
    // pg_wal_emitter awaits writes_scope.when_empty + submits the fsync.
    for (size_t i = 0; first_fsync > i; ++i) {
        EXPECT_EQ(op_kind::write, log[i].kind);
        EXPECT_EQ(pg_component::wal, log[i].component_id);
    }

    // 50 MiB/s at 8 KiB blocks over 200 ms = ~1280 expected writes in
    // the ideal Poisson stream. Floor the assertion well below that to
    // tolerate seed-driven variance while still proving the cycle is
    // actually under load -- a trivial [write, fsync] pair would slip
    // past the structural checks above.
    EXPECT_GT(first_fsync, 500u) << "expected a meaningful number of writes per cycle";
}

// Successive fsync cycles capture intended_ts at multiples of fsync_every_ms.
// Validates that last_fsync_ns advances correctly across cycles and that the
// "intended_ts is fsync_due time, not post-drain wall time" invariant holds
// for every cycle, not just the first.
TEST(pg_wal, successive_cycles_capture_due_intended_ts) {
    test_workload_ctx ctx;

    constexpr uint64_t k_bytes_per_sec = 50ull << 20;
    constexpr uint32_t k_block_size    = 8192;
    constexpr uint32_t k_fsync_ms      = 200;
    constexpr uint64_t k_fsync_due_ns  = uint64_t(k_fsync_ms) * 1'000'000ULL;
    constexpr uint32_t k_cycles        = 3;

    exec::async_scope scope;
    scope.spawn(pg_wal_emitter(ctx, /*region_offset=*/0, /*region_size=*/1ULL << 30, k_bytes_per_sec, k_fsync_ms,
                               /*fsync_every_writes=*/0, k_block_size, /*seed=*/4242));

    // Chunked stepping (see step_chunked rationale in test_workload_ctx).
    // Margin of one extra cycle so the kth fsync_due gate has a chance to
    // observe now >= deadline before we wind the run down.
    ctx.step_chunked(std::chrono::nanoseconds{k_fsync_due_ns * (k_cycles + 1)}, std::chrono::milliseconds{1});

    ctx.stop();
    ctx.scheduler().step(std::chrono::milliseconds{1});

    stdexec::sync_wait(scope.when_empty(stdexec::just()));

    auto const& log = ctx.log();

    // Collect every fsync. The production semantics (mirrored by the
    // synchronous test_ctx submit_op) advances last_fsync_ns by intended
    // + completion latency each cycle, so cycles drift forward by
    // whatever drain cost the previous cycle saw. That's exactly the
    // PostgreSQL commit-discipline behavior we want to lock in.
    //
    // Invariant under test: cycle i's fsync_due_ns == cycle (i-1)'s
    // fsync issue time + fsync_every_ms. The first cycle's reference is
    // ctx.now_ns() at emitter construction, which is 0.
    struct fsync_entry {
        uint64_t intended_ts;
        uint64_t issued_ts;
    };
    std::vector< fsync_entry > fsyncs;
    for (auto const& e : log) {
        if (op_kind::fsync == e.kind) { fsyncs.push_back({e.intended_ts_ns, e.issued_ts_ns}); }
    }
    ASSERT_GE(fsyncs.size(), k_cycles) << "expected at least one fsync per cycle";

    EXPECT_EQ(k_fsync_due_ns, fsyncs[0].intended_ts) << "first cycle's fsync_due is 0 + fsync_every_ms";
    for (uint32_t i = 1; k_cycles > i; ++i) {
        EXPECT_EQ(fsyncs[i - 1].issued_ts + k_fsync_due_ns, fsyncs[i].intended_ts)
            << "cycle " << i << " fsync_due should be previous fsync's issue time + fsync_every_ms";
    }
}

// Drain-ordering invariant: pg_wal_emitter MUST await
// writes_scope.when_empty before issuing the cycle's fsync. With
// synchronous submit_op the assertion is vacuous because the
// writes_scope is always empty by the time the emitter checks; with the
// test_ctx's parked-completion mode we keep wal.Write completions held
// while virtual time advances past the fsync deadline, so a regression
// that drops the drain await would let fsync issue while writes are
// still in-flight.
//
// Failure mode this guards against: removing
// `co_await writes_scope.when_empty(...)` from pg_wal_emitter.
TEST(pg_wal, fsync_blocks_on_writes_drain) {
    test_workload_ctx ctx;

    constexpr uint64_t k_bytes_per_sec = 50ull << 20;
    constexpr uint32_t k_block_size    = 8192;
    constexpr uint32_t k_fsync_ms      = 200;
    constexpr uint64_t k_fsync_due_ns  = uint64_t(k_fsync_ms) * 1'000'000ULL;

    // Park wal.Write specifically. wal.Fsync (and any other op kind a
    // future profile change might add) flows through synchronously, so
    // the test sees fsync the moment the emitter actually issues it --
    // not earlier, not later.
    ctx.hold_when([](billet::workload::op const& o) {
        return billet::workload::op_kind::write == o.kind && pg_component::wal == o.component_id;
    });

    exec::async_scope scope;
    scope.spawn(pg_wal_emitter(ctx, /*region_offset=*/0, /*region_size=*/1ULL << 30, k_bytes_per_sec, k_fsync_ms,
                               /*fsync_every_writes=*/0, k_block_size, /*seed=*/7777));

    // Step well past the cycle's fsync deadline in chunks. The emitter
    // spawns wal.Writes (all parked, accumulating in writes_scope) and
    // eventually hits the fsync_due branch where it awaits
    // writes_scope.when_empty. That await blocks because the parked
    // submit_op completions are what would let submit_and_forget
    // unwind out of writes_scope.
    ctx.step_chunked(std::chrono::nanoseconds{k_fsync_due_ns + k_fsync_due_ns / 2}, std::chrono::milliseconds{1});

    // Cycle is in the drain wait: writes are parked, fsync hasn't
    // issued yet.
    ASSERT_GT(ctx.pending_count(), 0u) << "expected wal.Writes to be parked while drain await is pending";

    auto count_fsyncs = [&]() {
        size_t n = 0;
        for (auto const& e : ctx.log()) {
            if (op_kind::fsync == e.kind) { ++n; }
        }
        return n;
    };
    ASSERT_EQ(0u, count_fsyncs()) << "fsync must not issue until writes_scope drains";

    // Clear the predicate before releasing so the post-fsync emitter
    // doesn't re-park cycle 2's writes; we only care about the cycle
    // boundary the test set up.
    ctx.hold_when(nullptr);
    ctx.release_all_pending();

    // The emitter is suspended on writes_scope.when_empty. Releasing
    // the parked writes resumes the submit_and_forget tasks, they
    // co_return out of writes_scope, when_empty resolves, and the
    // emitter issues the fsync. Step a small amount in case any
    // resumption schedules a follow-up timer (none expected, but
    // harmless).
    ctx.scheduler().step(std::chrono::microseconds{1});

    auto const& log = ctx.log();
    size_t first_fsync = log.size();
    for (size_t i = 0; log.size() > i; ++i) {
        if (op_kind::fsync == log[i].kind) {
            first_fsync = i;
            break;
        }
    }
    ASSERT_LT(first_fsync, log.size()) << "fsync should be issued after the drain releases";
    EXPECT_EQ(pg_component::wal, log[first_fsync].component_id);
    EXPECT_EQ(k_fsync_due_ns, log[first_fsync].intended_ts_ns)
        << "fsync's intended_ts is captured at fsync_due time, before the drain wait";

    // Wind down the emitter cleanly so async_scope drains.
    ctx.stop();
    ctx.scheduler().step(std::chrono::milliseconds{1});
    stdexec::sync_wait(scope.when_empty(stdexec::just()));
}

// Count-based fsync: with fsync_every_ms=0 and fsync_every_writes=N, an
// fsync should fire after exactly N writes accumulate, not before, not
// after. Mirrors the time-based test's intent for the other fsync
// discipline path.
TEST(pg_wal, count_based_fsync_fires_at_threshold) {
    test_workload_ctx ctx;

    constexpr uint64_t k_bytes_per_sec      = 50ULL << 20; // 50 MiB/s
    constexpr uint32_t k_block_size         = 8192;
    constexpr uint32_t k_fsync_every_ms     = 0; // disable time-based
    constexpr uint32_t k_fsync_every_writes = 100;

    exec::async_scope scope;
    scope.spawn(pg_wal_emitter(ctx, /*region_offset=*/0, /*region_size=*/1ULL << 30, k_bytes_per_sec,
                               k_fsync_every_ms, k_fsync_every_writes, k_block_size, /*seed=*/9999));

    // 50 MiB/s / 8 KiB = 6,400 writes/s -> ~64 fsync cycles in 1 s.
    // Chunked stepping so writes_issued accumulates incrementally rather
    // than letting all 6,400 writes fire at the same post-step now_ns.
    ctx.step_chunked(std::chrono::seconds{1}, std::chrono::milliseconds{1});

    ctx.stop();
    ctx.scheduler().step(std::chrono::milliseconds{1});
    stdexec::sync_wait(scope.when_empty(stdexec::just()));

    auto const& log = ctx.log();

    // Walk the log: every time a wal.Fsync appears, the running count of
    // wal.Write entries since the previous fsync should equal exactly
    // fsync_every_writes. A partial trailing cycle (writes with no
    // following fsync) is expected when stop fires mid-cycle and does
    // not violate the invariant -- the loop only asserts at fsync
    // boundaries.
    size_t writes_since_fsync = 0;
    size_t fsync_count        = 0;
    for (auto const& e : log) {
        if (op_kind::fsync == e.kind && pg_component::wal == e.component_id) {
            EXPECT_EQ(k_fsync_every_writes, writes_since_fsync)
                << "fsync " << fsync_count << " should fire after exactly "
                << k_fsync_every_writes << " wal writes";
            writes_since_fsync = 0;
            ++fsync_count;
        } else if (op_kind::write == e.kind && pg_component::wal == e.component_id) {
            ++writes_since_fsync;
        }
    }

    EXPECT_GE(fsync_count, 50u) << "expected several count-based cycles in a 1s run";
}

} // namespace
