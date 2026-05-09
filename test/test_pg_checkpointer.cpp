#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

#include <billet/workload/postgresql.hpp>
#include <workload/postgresql_impl.hpp>

#include "test_workload_ctx.hpp"

namespace {

using billet::test::test_workload_ctx;
using billet::workload::op_kind;
using billet::workload::profiles::detail::pg_checkpointer_emitter;
namespace pg_component = billet::workload::profiles::pg_component;

// Phase 4a explicitly fixed a "ckpt burst can exceed configured
// burst_bytes by up to qd*block_size" rounding bug by converting to a
// block count and distributing it exactly across qd sub-tasks (first
// `remainder` get one extra block). That fix needs a regression test:
// run one burst, count emitted ckpt.Write entries, assert it matches
// exactly burst_bytes / block_size.
TEST(pg_checkpointer, burst_emits_exactly_configured_blocks) {
    test_workload_ctx ctx;

    constexpr uint64_t k_device_size = 1ULL << 30; // 1 GiB
    constexpr uint32_t k_period_ms   = 1000;
    constexpr uint32_t k_block_size  = 65536;     // 64 KiB
    constexpr uint64_t k_burst_bytes = 16ULL << 20; // 16 MiB
    constexpr uint32_t k_qd          = 32;
    constexpr uint64_t k_seed        = 4242;
    constexpr uint64_t k_burst_blocks = k_burst_bytes / k_block_size; // 256

    exec::async_scope scope;
    scope.spawn(pg_checkpointer_emitter(ctx, k_device_size, k_period_ms, k_burst_bytes, k_block_size, k_seed, k_qd));

    // Step exactly to the first period boundary so one burst fires.
    // drain_expired pops the timer at k_period_ms, the emitter resumes
    // and runs the entire burst inline (every sub-task's submit_op is
    // synchronous in test_ctx), then suspends on schedule_at for the
    // next period.
    ctx.scheduler().step(std::chrono::milliseconds{k_period_ms});

    // Wind down: flip stopped, then advance past the next period so the
    // suspended schedule_at fires and the emitter observes stopped at
    // the loop top. Without this the coroutine stays parked on the
    // schedule_at sender and scope.when_empty would never resolve.
    ctx.stop();
    ctx.scheduler().step(std::chrono::milliseconds{k_period_ms + 1});

    stdexec::sync_wait(scope.when_empty(stdexec::just()));

    auto const& log = ctx.log();
    size_t      ckpt_writes = 0;
    for (auto const& e : log) {
        if (op_kind::write == e.kind && pg_component::ckpt == e.component_id) { ++ckpt_writes; }
    }
    EXPECT_EQ(k_burst_blocks, ckpt_writes)
        << "exactly burst_bytes / block_size writes per burst (not over, not under)";

    // Defensive: every emitted op is the configured block size. A
    // regression that re-introduced byte-based slicing would produce
    // writes with off-by-one sizes.
    for (auto const& e : log) {
        if (op_kind::write == e.kind && pg_component::ckpt == e.component_id) {
            EXPECT_EQ(k_block_size, e.len) << "all ckpt writes are exactly block_size";
        }
    }
}

} // namespace
