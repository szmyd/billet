#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#include <billet/workload/workload.hpp>
#include <engine/device_probe.hpp>
#include <engine/hdr.hpp>

namespace billet::engine {

// Engine-side configuration shared between the sender engine and the CLI.
// Lives here historically (file used to host the legacy callback engine);
// the types stay co-located with the sender engine since both consume them.
struct run_config {
    std::chrono::seconds duration{30};
    uint32_t qd{32};                 // per-worker queue depth
    uint32_t pool_block_size{4096};  // per-slot preallocated buffer size
    bool destructive_allowed{false}; // gates O_RDWR and Write/Discard/Fsync/WriteZeroes
};

// Lock-free counters published by all running workers. relaxed loads/stores
// are sufficient -- the consumer only needs an approximate snapshot, not
// strict ordering against the I/O hot path.
struct live_stats {
    std::atomic< uint64_t > ops{0};
    std::atomic< uint64_t > rx_bytes{0};
    std::atomic< uint64_t > tx_bytes{0};
    std::atomic< uint64_t > errors{0};
    std::atomic< uint32_t > inflight{0};
};

// Per-op-kind aggregate: ops + bytes counters and the merged HDR histogram
// of completion latencies for that kind. hdr_ptr owns the HDR via the
// stateless deleter declared in hdr.hpp.
struct op_data {
    uint64_t ops{0};
    uint64_t bytes{0};
    hdr_ptr hdr;
};

struct run_summary {
    uint64_t ops_completed{0};   // stored sum across by_kind
    uint64_t bytes_completed{0}; // stored sum across by_kind
    uint64_t errors{0};
    // Count of completions that bypassed per-cell accounting because the
    // emitter set component_id outside the profile's spec or set a (kind,
    // component) combination not declared by the spec. Should be 0 on a
    // sane run; a non-zero value means by_kind sums won't match the sum
    // of by_component_cell.
    uint64_t component_drops{0};
    uint32_t workers{0};
    std::chrono::nanoseconds elapsed{0};

    // Indexed by op_kind_index(kind). Empty kinds keep ops/bytes == 0
    // and a still-valid (empty) HDR so callers can iterate uniformly.
    std::array< op_data, billet::workload::k_op_kind_count > by_kind{};

    // One entry per (component, op_kind) cell declared by the profile's
    // component_spec span passed into run(). Order matches a fresh
    // make_cell_layout() over the same spec, so the CLI / report layer
    // can pair entries with their spec descriptors by walking spec + cells
    // together.
    std::vector< op_data > by_component_cell{};
};

// Emits the run summary via sisl::logging at INFO level.
void log_summary(device_info const& dev, run_config const& cfg, run_summary const& s);

} // namespace billet::engine
