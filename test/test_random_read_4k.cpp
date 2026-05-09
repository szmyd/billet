#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

#include <billet/workload/random_read_4k.hpp>
#include <workload/random_read_4k_impl.hpp>

#include "test_workload_ctx.hpp"

namespace {

using billet::test::test_workload_ctx;
using billet::workload::op_kind;
using billet::workload::detail::random_read_4k_run_impl;

// random_read_4k is the canonical closed-loop reader: spawn `qd` parallel
// coroutines, each issues one op, awaits it, issues the next. The
// closed-loop invariant is "at most `qd` ops are in flight at any moment"
// -- not because of an external semaphore (workload_ctx's qd gate is the
// production safety net) but because each coroutine serially awaits its
// own completion before issuing again.
//
// Test: park every read with the test_ctx hold predicate. The coroutines
// can only issue exactly one op before they're blocked on submit_op's
// set_value. After a small step, pending_count should be exactly qd --
// any larger means a coroutine somehow issued a second op without
// awaiting the first, breaking the closed-loop contract.
TEST(random_read_4k, in_flight_bounded_by_qd) {
    test_workload_ctx ctx;

    constexpr uint64_t k_device_size = 1ULL << 30; // 1 GiB
    constexpr uint32_t k_block_size  = 4096;
    constexpr uint32_t k_qd          = 16;
    constexpr uint64_t k_seed        = 7777;

    ctx.hold_when([](billet::workload::op const& o) { return billet::workload::op_kind::read == o.kind; });

    exec::async_scope scope;
    scope.spawn(random_read_4k_run_impl(ctx, k_device_size, k_block_size, k_qd, k_seed));

    // Step a tiny amount to let exec::async_scope kick the spawned
    // coroutines into their first co_await. Each parks immediately on
    // submit_op (the hold predicate matches). drain_expired runs
    // nothing because no timer was scheduled.
    ctx.scheduler().step(std::chrono::microseconds{1});

    EXPECT_EQ(k_qd, ctx.pending_count())
        << "expected exactly qd reads parked (one per closed-loop coroutine)";
    EXPECT_EQ(k_qd, ctx.log().size()) << "log records each issue; should be exactly qd";

    // Wind down: stop before releasing so each coroutine sees stopped
    // at its loop top after release fires set_value, and exits without
    // issuing a second op. Releasing without stop would leave the
    // coroutines looping forever.
    ctx.stop();
    ctx.release_all_pending();
    ctx.scheduler().step(std::chrono::milliseconds{1});

    stdexec::sync_wait(scope.when_empty(stdexec::just()));

    // Closed-loop bound proven: total issued == qd. No coroutine
    // got ahead of its own awaited completion.
    EXPECT_EQ(k_qd, ctx.log().size()) << "exactly qd reads issued total (closed-loop respected)";
    EXPECT_EQ(0u, ctx.pending_count()) << "release_all_pending drained the queue";
}

} // namespace
