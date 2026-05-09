#pragma once

#include <chrono>
#include <expected>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <bit>

#include <liburing.h>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <sisl/async/io_uring_scheduler.hpp>
#include <sisl/logging/logging.h>

#include <engine/aligned_buf.hpp>
#include <engine/device_probe.hpp>
#include <engine/hdr.hpp>
#include <engine/ring_worker.hpp>
#include <engine/sender_workload_ctx.hpp>
#include <stats/stats.hpp>

namespace billet::engine {

namespace detail {

inline std::error_condition errc_from(int errnum) noexcept {
    return std::generic_category().default_error_condition(errnum);
}

inline void pin_cpu(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    int const rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpuset), &cpuset);
    if (0 != rc) { LOGWARN("worker: pthread_setaffinity_np(cpu={}) failed: {}", cpu, rc); }
}

} // namespace detail

// Builder: callable taking workload_ctx& and returning exec::task<void>.
// The engine calls builder(ctx), spawns the result into an async_scope,
// and drives poll_once until the task completes.
//
// Single worker pinned to CPU 0 (multi-worker is a roadmap item).
// live_stats is fed by submit_op so the CLI's progress bar renders;
// by_component_cell + component_drops are accumulated via the
// cell_layout derived from `components`. An optional stats::group
// receives per-op records for the Prometheus exposition path.
//
// Ring sizing: cfg.qd device ops + k_control_overhead for timer / control
// SQEs. Buffer pool reserves cfg.qd buffers of cfg.pool_block_size up
// front so submit_op never allocates on the hot path.
template < typename Builder >
std::expected< run_summary, std::error_condition >
run(device_info const& dev, run_config const& cfg, std::span< billet::workload::component_spec const > components,
    live_stats* live, Builder&& builder, billet::stats::group* metrics = nullptr) {
    if (0 == cfg.qd) { return std::unexpected(std::make_error_condition(std::errc::invalid_argument)); }
    if (0 == cfg.pool_block_size || !std::has_single_bit(cfg.pool_block_size)) {
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }
    if (0 != dev.logical_block_size && cfg.pool_block_size < dev.logical_block_size) {
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }

    int const open_flags = O_DIRECT | O_CLOEXEC | (cfg.destructive_allowed ? O_RDWR : O_RDONLY);
    int const fd         = ::open(dev.path.c_str(), open_flags);
    if (0 > fd) { return std::unexpected(detail::errc_from(errno)); }

    // Ring capacity = qd device ops + small overhead for timer / control SQEs.
    constexpr uint32_t k_control_overhead = 16;
    uint32_t           ring_capacity      = 1;
    while (ring_capacity < cfg.qd + k_control_overhead) { ring_capacity <<= 1; }

    ::io_uring ring{};
    if (int const rc = ::io_uring_queue_init(ring_capacity, &ring, 0); 0 != rc) {
        ::close(fd);
        return std::unexpected(detail::errc_from(-rc));
    }

    // Buffer pool aligned to device physical block.
    uint32_t alignment = dev.physical_block_size;
    if (0 == alignment) { alignment = dev.logical_block_size; }
    if (0 == alignment) { alignment = 4096; }
    aligned_buf_pool pool(alignment);
    pool.reserve(cfg.pool_block_size, cfg.qd);

    auto const layout = billet::workload::make_cell_layout(components);

    workload_ctx::accum acc{};
    for (size_t k = 0; billet::workload::k_op_kind_count > k; ++k) {
        acc.hdrs[k] = make_hdr();
        if (!acc.hdrs[k]) {
            ::io_uring_queue_exit(&ring);
            ::close(fd);
            return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
        }
    }
    if (0 < layout.cell_count) {
        acc.cell_ops.resize(layout.cell_count, 0);
        acc.cell_bytes.resize(layout.cell_count, 0);
        acc.cell_hdrs.resize(layout.cell_count);
        for (size_t c = 0; layout.cell_count > c; ++c) {
            acc.cell_hdrs[c] = make_hdr();
            if (!acc.cell_hdrs[c]) {
                ::io_uring_queue_exit(&ring);
                ::close(fd);
                return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
            }
        }
    }

    sisl::async::io_uring_scheduler sched(&ring);

    using clock_type = std::chrono::steady_clock;
    auto const start = clock_type::now();
    uint64_t const deadline_ns =
        static_cast< uint64_t >(std::chrono::duration_cast< std::chrono::nanoseconds >(
                                     (start + cfg.duration).time_since_epoch())
                                     .count());

    workload_ctx ctx(sched, pool, fd, deadline_ns, acc, layout, cfg.qd, live, metrics);

    detail::pin_cpu(0);

    // Spawn the workload's run task. The builder is captured by reference;
    // it runs once, returns when its internal coroutines drain (typically
    // after ctx.stopped() flips).
    bool              done = false;
    exec::async_scope scope;
    auto              driver = [&]() -> exec::task< void > {
        co_await builder(ctx);
        done = true;
    };
    scope.spawn(driver());

    // Drive the io_uring scheduler until the driver task completes. Short
    // wait_budget keeps us responsive to the deadline check inside
    // ctx.stopped() so the loop tears down promptly when time runs out.
    while (!done) {
        sched.poll_once(std::chrono::milliseconds{1});
    }

    auto const elapsed = clock_type::now() - start;

    ::io_uring_queue_exit(&ring);
    ::close(fd);

    run_summary sum{};
    sum.workers          = 1;
    sum.elapsed          = elapsed;
    sum.errors           = acc.errors;
    sum.component_drops  = acc.component_drops;
    if (0 < layout.cell_count) {
        sum.by_component_cell.resize(layout.cell_count);
        for (size_t c = 0; layout.cell_count > c; ++c) {
            sum.by_component_cell[c].ops   = acc.cell_ops[c];
            sum.by_component_cell[c].bytes = acc.cell_bytes[c];
            sum.by_component_cell[c].hdr   = std::move(acc.cell_hdrs[c]);
        }
    }
    for (size_t k = 0; billet::workload::k_op_kind_count > k; ++k) {
        sum.by_kind[k].ops   = acc.ops_by_kind[k];
        sum.by_kind[k].bytes = acc.bytes_by_kind[k];
        sum.by_kind[k].hdr   = std::move(acc.hdrs[k]);
        sum.ops_completed += acc.ops_by_kind[k];
        sum.bytes_completed += acc.bytes_by_kind[k];
    }
    return sum;
}

} // namespace billet::engine
