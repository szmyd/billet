// Phase 0b smoke for sisl::async::io_uring_scheduler.
//
// Validates the pieces billet's later workload refactor depends on:
//   - schedule_at(wall_ns) wakes after the requested wall time, with the
//     wakeup arriving via the io_uring CQE path.
//   - async_submit(prep) submits an arbitrary SQE (here: NOP) and
//     completes with cqe->res when poll_once reaps it.
//
// Single-thread driver loop: the test thread submits, polls, and gets
// resumed -- the scheduler's contract. Cross-thread submit + poll is not
// supported (io_uring's SQ is not thread-safe by default).
//
// We can't use stdexec::sync_wait here -- it would block the same thread
// that needs to call poll_once, deadlocking the wakeup. Instead we
// start_detached the task and loop poll_once until a flag set inside the
// coroutine flips.

#include <chrono>
#include <ctime>

#include <gtest/gtest.h>

#include <liburing.h>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <sisl/async/io_uring_scheduler.hpp>

namespace {

// Sanity: IORING_OP_TIMEOUT itself fires and cqe_state encode/decode work,
// completely independent of coroutines / stdexec. If the next two tests
// hang, this one passing indicates the issue is in the coroutine glue, not
// the io_uring primitive.
TEST(io_uring_scheduler, raw_timeout_sqe_fires_and_cqe_state_round_trips) {
    ::io_uring ring{};
    ASSERT_EQ(0, ::io_uring_queue_init(8, &ring, 0));

    sisl::async::cqe_state    state;
    __kernel_timespec const   ts{.tv_sec = 0, .tv_nsec = 50'000'000}; // 50ms relative
    ::io_uring_sqe* const     sqe = ::io_uring_get_sqe(&ring);
    ::io_uring_prep_timeout(sqe, &ts, 0, 0); // no ABS flag: relative timeout
    ::io_uring_sqe_set_data64(sqe, sisl::async::encode_managed_user_data(&state));

    auto const t0 = std::chrono::steady_clock::now();
    ASSERT_EQ(1, ::io_uring_submit(&ring));

    ::io_uring_cqe* cqe = nullptr;
    ASSERT_EQ(0, ::io_uring_wait_cqe(&ring, &cqe));
    auto const elapsed = std::chrono::steady_clock::now() - t0;

    uint64_t const ud = ::io_uring_cqe_get_data64(cqe);
    ASSERT_TRUE(sisl::async::is_managed_user_data(ud));
    auto* const decoded = static_cast< sisl::async::cqe_state* >(sisl::async::decode_managed_user_data(ud));
    EXPECT_EQ(&state, decoded);
    EXPECT_GE(elapsed, std::chrono::milliseconds{45});

    ::io_uring_cqe_seen(&ring, cqe);
    ::io_uring_queue_exit(&ring);
}

TEST(io_uring_scheduler, schedule_at_wakes_after_wall_time) {
    ::io_uring ring{};
    ASSERT_EQ(0, ::io_uring_queue_init(8, &ring, 0));
    sisl::async::io_uring_scheduler sched(&ring);

    auto const t0 = std::chrono::steady_clock::now();
    // Use clock_gettime(CLOCK_MONOTONIC) for target_ns: steady_clock may map
    // to CLOCK_MONOTONIC_RAW on some toolchains, which diverges from the clock
    // io_uring uses for IORING_TIMEOUT_ABS.
    struct timespec now_ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &now_ts);
    uint64_t const target_ns = static_cast< uint64_t >(now_ts.tv_sec) * 1'000'000'000ULL
                              + static_cast< uint64_t >(now_ts.tv_nsec) + 50'000'000ULL;

    bool done = false;
    auto runner = [&]() -> exec::task< void > {
        co_await sched.schedule_at(target_ns);
        done = true;
    };
    exec::async_scope scope;
    scope.spawn(runner());

    while (!done) {
        sched.poll_once(std::chrono::milliseconds{10});
    }

    auto const elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, std::chrono::milliseconds{45})
        << "expected ~50ms wait, got "
        << std::chrono::duration_cast< std::chrono::milliseconds >(elapsed).count() << "ms";

    ::io_uring_queue_exit(&ring);
}

TEST(io_uring_scheduler, async_submit_nop_completes_with_zero) {
    ::io_uring ring{};
    ASSERT_EQ(0, ::io_uring_queue_init(8, &ring, 0));
    sisl::async::io_uring_scheduler sched(&ring);

    bool done   = false;
    int  result = -1;
    auto runner = [&]() -> exec::task< void > {
        result = co_await sched.async_submit([](::io_uring_sqe* sqe) { ::io_uring_prep_nop(sqe); });
        done   = true;
    };
    exec::async_scope scope;
    scope.spawn(runner());

    while (!done) {
        sched.poll_once(std::chrono::milliseconds{10});
    }
    EXPECT_EQ(0, result);

    ::io_uring_queue_exit(&ring);
}

} // namespace
