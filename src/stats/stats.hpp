#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sisl/metrics/metrics.hpp>

#include <billet/workload/workload.hpp>

namespace billet::stats {

// MetricsGroup wrapper that registers billet's hot-path counters and
// histograms with the sisl::metrics farm on construction. Engine TUs
// receive a pointer to this and call the inline per-op recording helpers
// on each CQE.
//
// Two registration tiers:
//   - Aggregate metrics (billet_ops_total, billet_op_latency_us, etc.)
//     registered statically via REGISTER_* macros for headline panels.
//   - Per-(component, op_kind) metrics emitted as Prom Family children
//     of three shared metric names -- billet_component_latency_us,
//     billet_component_ops_total, billet_component_bytes_total -- each
//     differentiated by a `cell="<component>.<OpKind>"` label (e.g.
//     cell="wal.Fsync"). Sparse: only cells declared by the profile's
//     component_spec span are registered, no invalid combos like
//     reader.Fsync. Dashboard queries decompose with `sum by(cell) (...)`,
//     which Prom 3 handles cleanly (a regex on __name__ with multi-name
//     match was broken inside rate() in 3.0.x).
class group : public sisl::MetricsGroup {
public:
    group(std::string const& run_id, std::span< billet::workload::component_spec const > components);
    ~group();

    // Per-completion hot path. URCU sharded counters / histograms make this
    // approximately a relaxed atomic add per worker -- safe to call at full
    // device IOPS without measurable per-op overhead.
    void on_op_complete(uint16_t component_id, billet::workload::op_kind kind, int64_t latency_ns,
                        uint64_t bytes) noexcept;
    void on_op_error() noexcept;

    // Absolute inflight count (sum across workers). Caller is expected to
    // sample from billet::engine::live_stats periodically rather than calling
    // per-op (gauges in sisl take an absolute value, not a delta).
    void set_inflight(int64_t total) noexcept;

private:
    // Per-(component, op_kind) metric indices (sisl token returns from
    // register_histogram / register_counter). -1 means "this cell is not in
    // the spec" -- on_op_complete drops the per-cell observation in that
    // case (per-kind aggregate path still runs).
    std::vector< std::array< int32_t, billet::workload::k_op_kind_count > > _cell_lat_idx;
    std::vector< std::array< int32_t, billet::workload::k_op_kind_count > > _cell_ops_idx;
    std::vector< std::array< int32_t, billet::workload::k_op_kind_count > > _cell_bytes_idx;
};

// HTTP server that exposes /metrics from sisl::MetricsFarm. RAII; stops on
// destruction. The 30s post-run drain is the caller's responsibility via
// drain().
class server {
public:
    explicit server(uint16_t port);
    ~server();

    server(server const&)            = delete;
    server& operator=(server const&) = delete;

    // Block for `dur` so a Prom scrape interval can pick up final counters.
    // Independent of the destructor so callers can sequence the drain
    // before destroying the metrics group.
    void drain(std::chrono::seconds dur);

private:
    struct impl;
    std::unique_ptr< impl > _p;
};

// Periodically copies an atomic gauge source (e.g. live_stats::inflight) into
// a billet::stats::group gauge. Owns its own thread; RAII tear-down. Cadence
// matches Prometheus' practical scrape interval -- a fast sampler would just
// burn CPU between scrapes.
class sampler {
public:
    sampler(group& g, std::atomic< uint32_t > const& inflight_src, std::chrono::milliseconds period);
    ~sampler();

    sampler(sampler const&)            = delete;
    sampler& operator=(sampler const&) = delete;

private:
    void run() noexcept;

    group&                            _g;
    std::atomic< uint32_t > const&    _src;
    std::chrono::milliseconds         _period;
    std::atomic< bool >               _done{false};
    std::thread                       _thread;
};

} // namespace billet::stats
