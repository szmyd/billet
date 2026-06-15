#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace billet::report {

struct host_info {
    std::string hostname;
    std::string kernel;
    std::string cpu_model;
    uint32_t cores{0};
    double ram_gb{0.0};
};

struct device_info {
    std::string path;
    uint64_t size_bytes{0};
    uint32_t logical_block{0};
    uint32_t physical_block{0};
    uint32_t max_io_kb{0};
    bool rotational{false};
    bool discard_supported{false};
    bool fua_supported{false};
    bool write_zeroes_supported{false};
    std::string label;
};

struct profile_info {
    std::string name;
    std::string version;
    std::map< std::string, std::string > params;
};

struct engine_info {
    std::string name;
    uint32_t qd_per_worker{0};
    uint32_t workers{0};
    uint32_t pin_cpu{0};
    std::string pin_strategy; // "mq" | "numa" | "linear" | "none"
    bool sqpoll{false};
    bool o_direct{true};
};

struct summary_stats {
    uint64_t ops_total{0};
    uint64_t bytes_total{0};
    double iops_mean{0.0};
    double throughput_mibs{0.0};
    uint64_t errors{0};
    // Internal accounting drops: a non-zero value here flags a profile/spec
    // mismatch and means by_op totals will not equal sum(by_component cells
    // with the same kind). 0 on a sane run.
    uint64_t component_drops{0};
};

struct op_stats {
    uint64_t count{0};
    uint64_t bytes{0};
    int64_t p50_us{0};
    int64_t p99_us{0};
    int64_t p99_9_us{0};
    int64_t p99_99_us{0};
    int64_t max_us{0};
    std::string hdr_b64;
};

struct run_results {
    summary_stats summary;
    std::map< std::string, op_stats > by_op;
    std::map< std::string, op_stats > by_phase;     // legacy schema slot; not currently emitted
    std::map< std::string, op_stats > by_component; // keys: "<component>.<OpKind>"
};

struct run_record {
    std::string schema_version;
    std::string run_id;
    std::string started_at;
    double duration_s{0.0};
    host_info host;
    device_info device;
    profile_info profile;
    engine_info engine;
    run_results results;
};

} // namespace billet::report
