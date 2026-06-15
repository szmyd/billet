#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace billet::engine {

// A blk-mq hardware queue and the logical CPUs blk-mq maps onto it, read from
// /sys/class/block/<disk>/mq/<id>/cpu_list. Submitting I/O from any CPU in
// `cpus` lands on this hardware queue, so pinning a worker to `cpus` drives a
// distinct queue. On a box with as many queues as cores this is one CPU per
// queue; on a larger server each queue owns a range of CPUs.
struct io_queue {
    uint32_t id{0};
    std::vector< uint32_t > cpus; // sorted, de-duplicated
};

// blk-mq + NUMA topology for one block device, discovered from sysfs. Best
// effort: synthetic or stacked devices (md, dm, loop) expose no mq directory,
// so `queues` stays empty and placement falls back to NUMA / linear policies.
struct device_topology {
    std::string disk;                  // resolved parent disk, e.g. "nvme0n1"
    int numa_node{-1};                 // device NUMA node; -1 == none/unknown
    std::vector< io_queue > queues;    // blk-mq hardware queues (empty if none)
    std::vector< uint32_t > node_cpus; // CPUs local to the device node, or all online
    uint32_t online_cpus{0};
    bool has_mq{false};
};

// Worker-to-CPU placement policy.
enum class pin_strategy {
    automatic, // mq -> numa -> linear, whichever the topology supports
    mq,        // pin each worker to a distinct hardware-queue cpuset
    numa,      // spread workers across the device-local NUMA node CPUs
    linear,    // worker i -> (base_cpu + i) % online_cpus
    none,      // do not pin
};

// One worker's CPU assignment, produced by plan_workers().
struct worker_placement {
    uint32_t worker_id{0};
    std::vector< uint32_t > cpus; // cpuset to pin to; empty == unpinned
    int32_t queue_id{-1};         // hardware queue targeted, -1 if n/a
    int numa_node{-1};
};

struct placement_plan {
    std::vector< worker_placement > workers;
    // The strategy actually applied. Never `automatic`: plan_workers resolves
    // it to a concrete policy (and may downgrade an explicit `mq` request when
    // the device has no hardware queues).
    pin_strategy strategy{pin_strategy::none};
};

// Parse a Linux cpu-list ("0-3,8,10-11") into a sorted, de-duplicated vector.
// Malformed tokens are skipped rather than treated as fatal.
std::vector< uint32_t > parse_cpu_list(std::string const& s);

// Inverse of parse_cpu_list: render a sorted cpu set as a compact range string
// ("0-3,8"). Empty set renders as "-".
std::string format_cpu_list(std::vector< uint32_t > const& cpus);

char const* pin_strategy_name(pin_strategy s) noexcept;
std::optional< pin_strategy > parse_pin_strategy(std::string_view s) noexcept;

// Discover a device's blk-mq + NUMA topology from sysfs. Resolves a partition
// to its parent disk. Always returns a topology: missing attributes degrade to
// empty queues and all-online-CPU fallback rather than failing.
device_topology discover_topology(std::filesystem::path const& dev_path);

// Worker count that auto-sizing (--workers 0) should use: one per NUMA-local
// hardware queue, or one per local CPU when the device has no mq, capped at the
// online CPU count and floored at 1.
uint32_t auto_worker_count(device_topology const& topo);

// Build a per-worker CPU placement for `worker_count` workers. `strategy`
// selects the policy; `automatic` resolves to the best the topology supports.
// An explicit `mq` request degrades to numa / linear when no hardware queues
// exist. `base_cpu` is the starting CPU for the linear policy.
placement_plan plan_workers(device_topology const& topo, uint32_t worker_count, pin_strategy strategy,
                            uint32_t base_cpu);

// Human-readable topology dump appended to `--probe` output. One key: value
// per line, trailing newline, matching device_probe::to_string formatting.
std::string topology_to_string(device_topology const& topo);

} // namespace billet::engine
