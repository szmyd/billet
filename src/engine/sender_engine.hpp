#pragma once

#include <chrono>
#include <expected>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <bit>

#include <liburing.h>

#include <hdr/hdr_histogram.h>

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
#include <engine/topology.hpp>
#include <stats/stats.hpp>

namespace billet::engine {

namespace detail {

inline std::error_condition errc_from(int errnum) noexcept {
    return std::generic_category().default_error_condition(errnum);
}

// Pin the calling thread to `cpus`. Empty set (pin_strategy::none) leaves the
// thread unpinned. CPUs at or beyond CPU_SETSIZE are skipped rather than
// overflowing the fixed mask.
inline void pin_cpuset(std::span< uint32_t const > cpus) {
    if (cpus.empty()) { return; }
    cpu_set_t set;
    CPU_ZERO(&set);
    int pinned = 0;
    for (auto const c : cpus) {
        if (c < CPU_SETSIZE) {
            CPU_SET(c, &set);
            ++pinned;
        }
    }
    if (0 == pinned) { return; }
    int const rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    if (0 != rc) { LOGWARN("worker: pthread_setaffinity_np failed: {}", rc); }
}

// Allocate the per-worker accumulator's HDR histograms and per-cell vectors.
// Done on the coordinator thread before workers spawn so OOM fails the run
// fast rather than mid-flight on a worker. Returns false on allocation failure.
inline bool init_accum(workload_ctx::accum& acc, billet::workload::cell_layout const& layout) {
    for (size_t k = 0; billet::workload::k_op_kind_count > k; ++k) {
        acc.hdrs[k] = make_hdr();
        if (!acc.hdrs[k]) { return false; }
    }
    if (0 < layout.cell_count) {
        acc.cell_ops.resize(layout.cell_count, 0);
        acc.cell_bytes.resize(layout.cell_count, 0);
        acc.cell_hdrs.resize(layout.cell_count);
        for (size_t c = 0; layout.cell_count > c; ++c) {
            acc.cell_hdrs[c] = make_hdr();
            if (!acc.cell_hdrs[c]) { return false; }
        }
    }
    return true;
}

} // namespace detail

// Builder: callable taking (workload_ctx&, worker_idx, worker_count) and
// returning exec::task<void>. The engine calls builder(ctx, i, n) once per
// worker, spawns the result into that worker's async_scope, and drives
// poll_once until the task completes. The builder MUST be safe to invoke
// concurrently from every worker thread (it only reads captured config and
// constructs a coroutine); per-worker divergence (seeds, which components
// run) is the builder's responsibility via the worker_idx argument.
//
// Each worker owns an independent fd (O_DIRECT), io_uring + scheduler, aligned
// buffer pool, and accumulator, and is pinned to its placement cpuset so it
// drives a distinct blk-mq hardware queue. All workers feed the shared
// live_stats and (URCU-sharded) metrics group; per-worker accumulators are
// merged into one run_summary after join via hdr_add.
//
// Ring sizing per worker: cfg.qd device ops + k_control_overhead for timer /
// control SQEs. Each pool reserves cfg.qd buffers of cfg.pool_block_size up
// front so submit_op never allocates on the hot path.
template < typename Builder >
std::expected< run_summary, std::error_condition >
run(device_info const& dev, run_config const& cfg, std::span< billet::workload::component_spec const > components,
    placement_plan const& placement, live_stats* live, Builder&& builder, billet::stats::group* metrics = nullptr) {
    if (0 == cfg.qd) { return std::unexpected(std::make_error_condition(std::errc::invalid_argument)); }
    if (0 == cfg.pool_block_size || !std::has_single_bit(cfg.pool_block_size)) {
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }
    if (0 != dev.logical_block_size && cfg.pool_block_size < dev.logical_block_size) {
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }

    uint32_t const worker_count = std::max< uint32_t >(1, static_cast< uint32_t >(placement.workers.size()));
    int const open_flags = O_DIRECT | O_CLOEXEC | (cfg.destructive_allowed ? O_RDWR : O_RDONLY);

    // Preflight open so permission / O_RDWR failures surface immediately rather
    // than after every worker has already run its full duration. Workers open
    // their own fds; this one is just a fail-fast probe.
    {
        int const probe_fd = ::open(dev.path.c_str(), open_flags);
        if (0 > probe_fd) { return std::unexpected(detail::errc_from(errno)); }
        ::close(probe_fd);
    }

    // Ring capacity = qd device ops + small overhead for timer / control SQEs.
    constexpr uint32_t k_control_overhead = 16;
    uint32_t ring_capacity = 1;
    while (ring_capacity < cfg.qd + k_control_overhead) {
        ring_capacity <<= 1;
    }

    // Buffer pool alignment = device physical block (logical, then 4K fallback).
    uint32_t alignment = dev.physical_block_size;
    if (0 == alignment) { alignment = dev.logical_block_size; }
    if (0 == alignment) { alignment = 4096; }

    auto const layout = billet::workload::make_cell_layout(components);

    // Per-worker accumulators, HDRs allocated up front (fail fast on OOM).
    std::vector< workload_ctx::accum > accums(worker_count);
    for (auto& acc : accums) {
        if (!detail::init_accum(acc, layout)) {
            return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
        }
    }

    using clock_type = std::chrono::steady_clock;
    auto const start = clock_type::now();
    uint64_t const deadline_ns = static_cast< uint64_t >(
        std::chrono::duration_cast< std::chrono::nanoseconds >((start + cfg.duration).time_since_epoch()).count());

    // Per-worker init error slots. nullopt == the worker initialized and ran.
    std::vector< std::optional< std::error_condition > > init_errors(worker_count);

    // One worker thread per placement entry. Each is fully share-nothing on the
    // I/O hot path except for the shared live_stats / metrics sinks, which are
    // built for concurrent updates.
    auto const worker_body = [&](uint32_t idx) noexcept {
        detail::pin_cpuset(placement.workers[idx].cpus);

        int const fd = ::open(dev.path.c_str(), open_flags);
        if (0 > fd) {
            init_errors[idx] = detail::errc_from(errno);
            return;
        }

        ::io_uring ring{};
        if (int const rc = ::io_uring_queue_init(ring_capacity, &ring, 0); 0 != rc) {
            ::close(fd);
            init_errors[idx] = detail::errc_from(-rc);
            return;
        }

        try {
            aligned_buf_pool pool(alignment);
            pool.reserve(cfg.pool_block_size, cfg.qd);

            sisl::async::io_uring_scheduler sched(&ring);
            workload_ctx ctx(sched, pool, fd, deadline_ns, accums[idx], layout, cfg.qd, live, metrics);

            bool done = false;
            exec::async_scope scope;
            auto driver = [&]() -> exec::task< void > {
                co_await builder(ctx, idx, worker_count);
                done = true;
            };
            scope.spawn(driver());

            while (!done) {
                sched.poll_once(std::chrono::milliseconds{1});
            }
        } catch (std::bad_alloc const&) { init_errors[idx] = std::make_error_condition(std::errc::not_enough_memory); }

        ::io_uring_queue_exit(&ring);
        ::close(fd);
    };

    std::vector< std::thread > threads;
    threads.reserve(worker_count);
    for (uint32_t i = 0; worker_count > i; ++i) {
        threads.emplace_back(worker_body, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    auto const elapsed = clock_type::now() - start;

    // Any worker that failed to initialize invalidates the run.
    for (auto const& e : init_errors) {
        if (e) { return std::unexpected(*e); }
    }

    // Merge per-worker accumulators into one summary. HDRs share bucket
    // geometry (make_hdr), so hdr_add composes the distributions exactly.
    run_summary sum{};
    sum.workers = worker_count;
    sum.elapsed = elapsed;

    if (0 < layout.cell_count) {
        sum.by_component_cell.resize(layout.cell_count);
        for (size_t c = 0; layout.cell_count > c; ++c) {
            sum.by_component_cell[c].hdr = make_hdr();
        }
    }
    for (size_t k = 0; billet::workload::k_op_kind_count > k; ++k) {
        sum.by_kind[k].hdr = make_hdr();
    }

    for (auto& acc : accums) {
        sum.errors += acc.errors;
        sum.component_drops += acc.component_drops;
        for (size_t k = 0; billet::workload::k_op_kind_count > k; ++k) {
            sum.by_kind[k].ops += acc.ops_by_kind[k];
            sum.by_kind[k].bytes += acc.bytes_by_kind[k];
            sum.ops_completed += acc.ops_by_kind[k];
            sum.bytes_completed += acc.bytes_by_kind[k];
            if (sum.by_kind[k].hdr && acc.hdrs[k]) { ::hdr_add(sum.by_kind[k].hdr.get(), acc.hdrs[k].get()); }
        }
        for (size_t c = 0; layout.cell_count > c; ++c) {
            sum.by_component_cell[c].ops += acc.cell_ops[c];
            sum.by_component_cell[c].bytes += acc.cell_bytes[c];
            if (sum.by_component_cell[c].hdr && acc.cell_hdrs[c]) {
                ::hdr_add(sum.by_component_cell[c].hdr.get(), acc.cell_hdrs[c].get());
            }
        }
    }

    return sum;
}

} // namespace billet::engine
