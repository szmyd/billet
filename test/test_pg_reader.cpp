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
using billet::workload::profiles::detail::pg_reader_emitter;
namespace pg_component = billet::workload::profiles::pg_component;

// pg_reader's hot/cold partition is the load-bearing piece of the
// PostgreSQL backend-read model: hot_set_frac of the device is the
// "frequently accessed working set," locality is the probability any
// individual read targets it. With locality=0.85, ~85% of emitted reads
// should land in the first hot_blocks blocks, the rest in the cold tail.
//
// The test runs the emitter against virtual time for long enough to get
// a statistically meaningful sample (~100k ops at 10k IOPS over 10 s),
// buckets each emitted offset into hot or cold, and checks the observed
// hot fraction lands within tolerance of the configured locality.
TEST(pg_reader, hot_set_distribution_matches_locality) {
    test_workload_ctx ctx;

    constexpr uint64_t k_device_size  = 1ULL << 30; // 1 GiB
    constexpr uint32_t k_block_size   = 8192;
    constexpr uint32_t k_target_iops  = 10000;
    constexpr double   k_hot_set_frac = 0.10;
    constexpr double   k_locality     = 0.85;
    constexpr uint64_t k_seed         = 12345;

    exec::async_scope scope;
    scope.spawn(pg_reader_emitter(ctx, k_device_size, k_block_size, k_target_iops, k_hot_set_frac, k_locality,
                                   k_seed));

    // Chunked stepping so the C++ stack unwinds between batches. The
    // synchronous submit_op in test_ctx fires set_value inline from
    // start(), which resumes the awaiting coroutine, which immediately
    // hits the next submit_op -- everything on one growing stack. A
    // 10 s single jump emits ~100k events in one synchronous resumption
    // chain, which blows the stack under sanitizers (large per-frame
    // redzones). 1 ms chunks let the chain unwind ~6 emissions at a
    // time, keeping depth bounded.
    ctx.step_chunked(std::chrono::seconds{10}, std::chrono::milliseconds{1});

    ctx.stop();
    ctx.scheduler().step(std::chrono::milliseconds{1});
    stdexec::sync_wait(scope.when_empty(stdexec::just()));

    auto const&    log          = ctx.log();
    uint64_t const total_blocks = k_device_size / k_block_size;
    // Mirror the emitter's clamping logic exactly: at least 1 hot block,
    // and never larger than the device. With hot_set_frac=0.10 on 1 GiB /
    // 8 KiB, hot_blocks is 13107 -- comfortably above the floor and below
    // the ceiling.
    uint64_t const hot_blocks =
        std::min(total_blocks, std::max< uint64_t >(1, static_cast< uint64_t >(total_blocks * k_hot_set_frac)));

    size_t hot_count = 0;
    for (auto const& e : log) {
        if (op_kind::read != e.kind || pg_component::reader != e.component_id) { continue; }
        uint64_t const block_idx = e.offset / k_block_size;
        if (block_idx < hot_blocks) { ++hot_count; }
    }

    ASSERT_GT(log.size(), 1000u) << "expected a statistically meaningful sample";
    double const observed_locality = static_cast< double >(hot_count) / static_cast< double >(log.size());

    // 100k samples, true_p=0.85: sigma = sqrt(N*p*(1-p))/N ~= 0.001.
    // A 3% absolute tolerance is ~30 sigma -- generous against seed
    // variance without admitting a real distribution bug.
    EXPECT_NEAR(observed_locality, k_locality, 0.03)
        << "observed hot fraction should match configured locality";
}

} // namespace
