#include <hdr/hdr_histogram.h>
#include <sisl/logging/logging.h>

#include <engine/ring_worker.hpp>

namespace billet::engine {

using billet::workload::k_op_kind_count;

void log_summary(device_info const& dev, run_config const& cfg, run_summary const& s) {
    auto const   secs        = std::chrono::duration< double >(s.elapsed).count();
    double const iops        = (0.0 < secs) ? (static_cast< double >(s.ops_completed) / secs) : 0.0;
    double const mib_per_sec = (0.0 < secs) ? (static_cast< double >(s.bytes_completed) / 1048576.0 / secs) : 0.0;

    auto const us = [](int64_t ns) { return ns / 1000; };

    LOGINFO("device:      {}", dev.path.string());
    LOGINFO("duration:    {:.3f} s", secs);
    LOGINFO("workers:     {}", s.workers);
    LOGINFO("qd/worker:   {}", cfg.qd);
    LOGINFO("ops:         {}", s.ops_completed);
    LOGINFO("bytes:       {}", s.bytes_completed);
    LOGINFO("errors:      {}", s.errors);
    LOGINFO("iops:        {:.1f}", iops);
    LOGINFO("throughput:  {:.2f} MiB/s", mib_per_sec);

    if (0 < s.component_drops) {
        LOGWARN("internal: {} per-component accounting drops -- "
                "an emitter set component_id outside its profile's spec or "
                "a (component, kind) combination not declared by the spec. "
                "by_op totals are correct but by_component will not sum to by_op.",
                s.component_drops);
    }

    // Per-op-kind percentile blocks; skip kinds the workload didn't touch.
    for (size_t k = 0; k_op_kind_count > k; ++k) {
        auto const& d = s.by_kind[k];
        if (0 == d.ops) { continue; }
        auto const* const name = billet::workload::op_kind_name(static_cast< billet::workload::op_kind >(k));
        LOGINFO("{}: ops={} bytes={}", name, d.ops, d.bytes);
        if (d.hdr) {
            auto* const h = d.hdr.get();
            LOGINFO("  p50:    {} us", us(::hdr_value_at_percentile(h, 50.0)));
            LOGINFO("  p90:    {} us", us(::hdr_value_at_percentile(h, 90.0)));
            LOGINFO("  p99:    {} us", us(::hdr_value_at_percentile(h, 99.0)));
            LOGINFO("  p99.9:  {} us", us(::hdr_value_at_percentile(h, 99.9)));
            LOGINFO("  p99.99: {} us", us(::hdr_value_at_percentile(h, 99.99)));
            LOGINFO("  max:    {} us", us(::hdr_max(h)));
        }
    }
}

} // namespace billet::engine
