#include <stats/stats.hpp>

#include <thread>

#include <sisl/http/http_server.hpp>
#include <sisl/logging/logging.h>
#include <sisl/metrics/histogram_buckets.hpp>

namespace billet::stats {

group::group(std::string const& run_id, std::span< billet::workload::component_spec const > components) :
        sisl::MetricsGroup{"billet", run_id} {
    REGISTER_COUNTER(billet_ops_total, "Total ops completed");
    REGISTER_COUNTER(billet_bytes_total, "Total bytes read+written across completed ops");
    REGISTER_COUNTER(billet_errors_total, "Total CQEs with negative result");
    REGISTER_COUNTER(billet_component_drops_total,
                     "Completions that bypassed per-component accounting because the "
                     "emitter set component_id outside the spec or a (component, kind) "
                     "combination not declared by it. Should be 0 on a sane run.");
    REGISTER_GAUGE(billet_inflight, "Inflight ops summed across workers");
    REGISTER_HISTOGRAM(billet_op_latency_us, "Aggregate op completion latency, microseconds",
                       HistogramBucketsType(OpLatecyBuckets));
    REGISTER_HISTOGRAM(billet_op_size_bytes, "Aggregate op size, bytes",
                       HistogramBucketsType(OpSizeBuckets));

    // Per-(component, op_kind) sparse cell registration. Three metric
    // families are registered ONCE each at first encounter:
    //   billet_component_latency_us (histogram)
    //   billet_component_ops_total  (counter)
    //   billet_component_bytes_total (counter)
    // Each cell becomes a child series within the family, differentiated
    // by a `cell="<component>.<OpKind>"` label (e.g. cell="wal.Fsync").
    //
    // This pattern is what Prom 3 expects: queries like
    // `sum by(cell) (rate(billet_component_ops_total[30s]))` work without
    // regex-on-__name__ in range-vector functions, which is broken in
    // Prom 3.0 when matching multiple metric names. Cells not declared
    // by the spec stay at -1 in the dispatch table and are silently
    // ignored by on_op_complete.
    static constexpr char const* k_lat_name = "billet_component_latency_us";
    static constexpr char const* k_lat_desc = "Per-component completion latency, microseconds";
    static constexpr char const* k_ops_name = "billet_component_ops_total";
    static constexpr char const* k_ops_desc = "Per-component ops completed";
    static constexpr char const* k_bytes_name = "billet_component_bytes_total";
    static constexpr char const* k_bytes_desc = "Per-component bytes completed";

    _cell_lat_idx.resize(components.size());
    _cell_ops_idx.resize(components.size());
    _cell_bytes_idx.resize(components.size());
    for (auto& row : _cell_lat_idx) { row.fill(-1); }
    for (auto& row : _cell_ops_idx) { row.fill(-1); }
    for (auto& row : _cell_bytes_idx) { row.fill(-1); }

    for (size_t c = 0; components.size() > c; ++c) {
        auto const& spec = components[c];
        for (auto const kind : spec.kinds) {
            // cell="reader.Read", "wal.Fsync", etc. -- single label whose
            // value uniquely identifies the (component, kind) child.
            std::string const cell_value = std::string{spec.metric_name} + "." + billet::workload::op_kind_name(kind);
            sisl::metric_label const cell_label{"cell", cell_value};

            uint64_t const lat_idx = m_impl_ptr->register_histogram(k_lat_name, k_lat_desc, "", cell_label,
                                                                    HistogramBucketsType(OpLatecyBuckets));
            uint64_t const ops_idx = m_impl_ptr->register_counter(k_ops_name, k_ops_desc, "", cell_label);
            uint64_t const bytes_idx = m_impl_ptr->register_counter(k_bytes_name, k_bytes_desc, "", cell_label);

            auto const ki = billet::workload::op_kind_index(kind);
            _cell_lat_idx[c][ki] = static_cast< int32_t >(lat_idx);
            _cell_ops_idx[c][ki] = static_cast< int32_t >(ops_idx);
            _cell_bytes_idx[c][ki] = static_cast< int32_t >(bytes_idx);
        }
    }

    register_me_to_farm();
}

group::~group() { deregister_me_from_farm(); }

void group::on_op_complete(uint16_t component_id, billet::workload::op_kind kind, int64_t latency_ns,
                           uint64_t bytes) noexcept {
    auto const latency_us = latency_ns / 1000;

    COUNTER_INCREMENT(*this, billet_ops_total, 1);
    COUNTER_INCREMENT(*this, billet_bytes_total, bytes);
    HISTOGRAM_OBSERVE(*this, billet_op_latency_us, latency_us);
    if (0 < bytes) { HISTOGRAM_OBSERVE(*this, billet_op_size_bytes, bytes); }

    bool const in_range = component_id < _cell_lat_idx.size();
    auto const ki = billet::workload::op_kind_index(kind);
    int32_t const lat = in_range ? _cell_lat_idx[component_id][ki] : -1;
    int32_t const ops = in_range ? _cell_ops_idx[component_id][ki] : -1;
    int32_t const bts = in_range ? _cell_bytes_idx[component_id][ki] : -1;
    if (0 <= lat && 0 <= ops) {
        m_impl_ptr->histogram_observe(static_cast< uint64_t >(lat), latency_us);
        m_impl_ptr->counter_increment(static_cast< uint64_t >(ops), 1);
        if (0 <= bts && 0 < bytes) { m_impl_ptr->counter_increment(static_cast< uint64_t >(bts), bytes); }
    } else if (!_cell_lat_idx.empty()) {
        // Empty layout means per-cell accounting is disabled (single-component
        // profiles via empty span); only count drops when the layout exists
        // but this op was rejected by it.
        COUNTER_INCREMENT(*this, billet_component_drops_total, 1);
        DEBUG_ASSERT(0 <= lat, "component_id={} kind={} not in profile spec",
                     component_id, static_cast< int >(kind));
    }
}

void group::on_op_error() noexcept { COUNTER_INCREMENT(*this, billet_errors_total, 1); }

void group::set_inflight(int64_t total) noexcept { GAUGE_UPDATE(*this, billet_inflight, total); }

struct server::impl {
    sisl::HttpServer http;

    explicit impl(uint16_t port) : http{port} {
#ifdef PROMETHEUS_METRICS_REPORTER
        http.register_metrics_endpoint();
#endif
        http.start();
    }
    ~impl() { http.stop(); }
};

server::server(uint16_t port) : _p(std::make_unique< impl >(port)) {}
server::~server() = default;

void server::drain(std::chrono::seconds dur) {
    std::this_thread::sleep_for(dur);
}

sampler::sampler(group& g, std::atomic< uint32_t > const& inflight_src, std::chrono::milliseconds period) :
        _g(g), _src(inflight_src), _period(period), _thread([this] { run(); }) {}

sampler::~sampler() {
    _done.store(true, std::memory_order_release);
    if (_thread.joinable()) { _thread.join(); }
}

void sampler::run() noexcept {
    while (!_done.load(std::memory_order_acquire)) {
        _g.set_inflight(static_cast< int64_t >(_src.load(std::memory_order_relaxed)));
        std::this_thread::sleep_for(_period);
    }
    // Final sample so a scraper that races the run-end shutdown still sees
    // the terminal value (typically zero once workers drain).
    _g.set_inflight(static_cast< int64_t >(_src.load(std::memory_order_relaxed)));
}

} // namespace billet::stats
