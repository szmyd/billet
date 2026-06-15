#include <engine/topology.hpp>

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

namespace billet::engine {

namespace {

// Read the first whitespace-trimmed line of a sysfs file. Returns nullopt when
// the file is absent or unreadable so callers can degrade cleanly.
std::optional< std::string > read_sysfs_line(std::filesystem::path const& p) {
    std::ifstream f(p);
    if (!f) { return std::nullopt; }
    std::string line;
    std::getline(f, line);
    auto const first = line.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) { return std::string{}; }
    auto const last = line.find_last_not_of(" \t\r\n");
    return line.substr(first, last - first + 1);
}

std::optional< uint32_t > parse_u32(std::string_view tok) noexcept {
    uint32_t v = 0;
    auto const beg = tok.data();
    auto const end = tok.data() + tok.size();
    auto const r = std::from_chars(beg, end, v);
    if (r.ec != std::errc{} || r.ptr != end) { return std::nullopt; }
    return v;
}

// CPU count visible to the process, used as the linear-policy modulus and the
// auto-worker cap when sysfs does not expose an online mask.
uint32_t online_cpu_fallback() noexcept {
    long const n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return (0 < n) ? static_cast< uint32_t >(n) : 1u;
}

// Resolve /dev/<x> to its parent disk node under /sys/class/block. Partitions
// (which carry a `partition` attribute) resolve to the disk that owns them so
// the mq directory is found; bare disks resolve to themselves.
std::string resolve_disk(std::filesystem::path const& dev_path) {
    std::string const name = dev_path.filename().string();
    auto const block_dir = std::filesystem::path("/sys/class/block") / name;

    std::error_code ec;
    if (std::filesystem::exists(block_dir / "partition", ec)) {
        auto const real = std::filesystem::canonical(block_dir, ec);
        if (!ec) {
            // .../<disk>/<partition> -> <disk>
            auto const parent = real.parent_path().filename().string();
            if (!parent.empty()) { return parent; }
        }
    }
    return name;
}

bool intersects(std::vector< uint32_t > const& a, std::vector< uint32_t > const& b) {
    // Both are sorted; a linear merge would do, but these are tiny.
    for (auto const c : a) {
        if (std::binary_search(b.begin(), b.end(), c)) { return true; }
    }
    return false;
}

bool queue_is_local(io_queue const& q, device_topology const& topo) {
    // numa_node < 0 means the device declares no NUMA affinity, so every queue
    // is equally local.
    if (0 > topo.numa_node) { return true; }
    return intersects(q.cpus, topo.node_cpus);
}

// Queues ordered for placement: NUMA-local first, then the rest, each group in
// ascending hardware-queue id so assignment is deterministic across runs.
std::vector< io_queue const* > ordered_queues(device_topology const& topo) {
    std::vector< io_queue const* > local;
    std::vector< io_queue const* > remote;
    for (auto const& q : topo.queues) {
        if (queue_is_local(q, topo)) {
            local.push_back(&q);
        } else {
            remote.push_back(&q);
        }
    }
    local.insert(local.end(), remote.begin(), remote.end());
    return local;
}

pin_strategy resolve_strategy(device_topology const& topo, pin_strategy requested) {
    auto const best = [&]() -> pin_strategy {
        if (topo.has_mq && !topo.queues.empty()) { return pin_strategy::mq; }
        if (!topo.node_cpus.empty()) { return pin_strategy::numa; }
        return pin_strategy::linear;
    };

    switch (requested) {
    case pin_strategy::automatic:
        return best();
    case pin_strategy::mq:
        // Honor the request only if the device actually has queues; otherwise
        // downgrade to the best available so the run still pins sensibly.
        return (topo.has_mq && !topo.queues.empty()) ? pin_strategy::mq : best();
    case pin_strategy::numa:
        return (!topo.node_cpus.empty()) ? pin_strategy::numa : pin_strategy::linear;
    case pin_strategy::linear:
    case pin_strategy::none:
        return requested;
    }
    return pin_strategy::none;
}

} // namespace

std::vector< uint32_t > parse_cpu_list(std::string const& s) {
    std::vector< uint32_t > out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        auto const trim_b = tok.find_first_not_of(" \t");
        if (std::string::npos == trim_b) { continue; }
        auto const trim_e = tok.find_last_not_of(" \t");
        std::string_view const view{tok.data() + trim_b, trim_e - trim_b + 1};

        auto const dash = view.find('-');
        if (std::string_view::npos == dash) {
            if (auto const v = parse_u32(view)) { out.push_back(*v); }
            continue;
        }
        auto const lo = parse_u32(view.substr(0, dash));
        auto const hi = parse_u32(view.substr(dash + 1));
        if (!lo || !hi || *hi < *lo) { continue; }
        for (uint32_t c = *lo; *hi >= c; ++c) {
            out.push_back(c);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string format_cpu_list(std::vector< uint32_t > const& cpus) {
    if (cpus.empty()) { return "-"; }
    std::string out;
    size_t i = 0;
    while (cpus.size() > i) {
        size_t j = i;
        while (cpus.size() > j + 1 && cpus[j + 1] == cpus[j] + 1) {
            ++j;
        }
        if (!out.empty()) { out += ','; }
        out += std::to_string(cpus[i]);
        if (j > i) {
            out += '-';
            out += std::to_string(cpus[j]);
        }
        i = j + 1;
    }
    return out;
}

char const* pin_strategy_name(pin_strategy s) noexcept {
    switch (s) {
    case pin_strategy::automatic:
        return "auto";
    case pin_strategy::mq:
        return "mq";
    case pin_strategy::numa:
        return "numa";
    case pin_strategy::linear:
        return "linear";
    case pin_strategy::none:
        return "none";
    }
    return "none";
}

std::optional< pin_strategy > parse_pin_strategy(std::string_view s) noexcept {
    if ("auto" == s) { return pin_strategy::automatic; }
    if ("mq" == s) { return pin_strategy::mq; }
    if ("numa" == s) { return pin_strategy::numa; }
    if ("linear" == s) { return pin_strategy::linear; }
    if ("none" == s) { return pin_strategy::none; }
    return std::nullopt;
}

device_topology discover_topology(std::filesystem::path const& dev_path) {
    device_topology topo;
    topo.disk = resolve_disk(dev_path);
    topo.online_cpus = online_cpu_fallback();

    auto const block_dir = std::filesystem::path("/sys/class/block") / topo.disk;

    // blk-mq hardware queues: /sys/class/block/<disk>/mq/<id>/cpu_list.
    std::error_code ec;
    auto const mq_dir = block_dir / "mq";
    if (std::filesystem::is_directory(mq_dir, ec)) {
        for (auto const& entry : std::filesystem::directory_iterator(mq_dir, ec)) {
            if (!entry.is_directory()) { continue; }
            auto const qid = parse_u32(entry.path().filename().string());
            if (!qid) { continue; }
            io_queue q;
            q.id = *qid;
            if (auto const line = read_sysfs_line(entry.path() / "cpu_list")) { q.cpus = parse_cpu_list(*line); }
            topo.queues.push_back(std::move(q));
        }
        std::sort(topo.queues.begin(), topo.queues.end(),
                  [](io_queue const& a, io_queue const& b) { return a.id < b.id; });
        topo.has_mq = !topo.queues.empty();
    }

    // Device NUMA node (PCIe affinity for NVMe). "-1" or absent means none.
    if (auto const line = read_sysfs_line(block_dir / "device" / "numa_node")) {
        int node = -1;
        auto const r = std::from_chars(line->data(), line->data() + line->size(), node);
        if (r.ec == std::errc{}) { topo.numa_node = node; }
    }

    // Global online CPU mask: the linear-policy modulus and the auto-worker
    // cap. Highest online id + 1 is the right modulus even with holes.
    std::vector< uint32_t > online_all;
    if (auto const line = read_sysfs_line("/sys/devices/system/cpu/online")) { online_all = parse_cpu_list(*line); }
    if (!online_all.empty()) { topo.online_cpus = online_all.back() + 1; }

    // CPUs local to the device node, falling back to the global online mask
    // when the device has no NUMA affinity or the node file is missing.
    if (0 <= topo.numa_node) {
        auto const node_path =
            std::filesystem::path("/sys/devices/system/node") / ("node" + std::to_string(topo.numa_node)) / "cpulist";
        if (auto const line = read_sysfs_line(node_path)) { topo.node_cpus = parse_cpu_list(*line); }
    }
    if (topo.node_cpus.empty()) { topo.node_cpus = online_all; }

    return topo;
}

uint32_t auto_worker_count(device_topology const& topo) {
    uint32_t n = 1;
    if (topo.has_mq && !topo.queues.empty()) {
        uint32_t local = 0;
        for (auto const& q : topo.queues) {
            if (queue_is_local(q, topo)) { ++local; }
        }
        n = (0 < local) ? local : static_cast< uint32_t >(topo.queues.size());
    } else if (!topo.node_cpus.empty()) {
        n = static_cast< uint32_t >(topo.node_cpus.size());
    }
    uint32_t const cap = (0 < topo.online_cpus) ? topo.online_cpus : online_cpu_fallback();
    return std::clamp< uint32_t >(n, 1, cap);
}

placement_plan plan_workers(device_topology const& topo, uint32_t worker_count, pin_strategy strategy,
                            uint32_t base_cpu) {
    placement_plan plan;
    plan.strategy = resolve_strategy(topo, strategy);
    uint32_t const n = std::max< uint32_t >(1, worker_count);
    uint32_t const online = (0 < topo.online_cpus) ? topo.online_cpus : online_cpu_fallback();
    auto const ordered = (pin_strategy::mq == plan.strategy) ? ordered_queues(topo) : std::vector< io_queue const* >{};

    plan.workers.reserve(n);
    for (uint32_t i = 0; n > i; ++i) {
        worker_placement w;
        w.worker_id = i;
        w.numa_node = topo.numa_node;
        switch (plan.strategy) {
        case pin_strategy::mq: {
            io_queue const* const q = ordered[i % ordered.size()];
            w.cpus = q->cpus;
            w.queue_id = static_cast< int32_t >(q->id);
            break;
        }
        case pin_strategy::numa: {
            uint32_t const cpu = topo.node_cpus[i % topo.node_cpus.size()];
            w.cpus = {cpu};
            break;
        }
        case pin_strategy::linear: {
            uint32_t const cpu = (base_cpu + i) % online;
            w.cpus = {cpu};
            break;
        }
        case pin_strategy::automatic:
        case pin_strategy::none:
            break; // unpinned
        }
        plan.workers.push_back(std::move(w));
    }
    return plan;
}

std::string topology_to_string(device_topology const& topo) {
    std::ostringstream os;
    os << "disk:                   " << topo.disk << "\n"
       << "numa_node:              " << topo.numa_node << "\n"
       << "online_cpus:            " << topo.online_cpus << "\n"
       << "node_cpus:              " << format_cpu_list(topo.node_cpus) << "\n"
       << "hw_queues:              " << topo.queues.size() << "\n";
    for (auto const& q : topo.queues) {
        os << "  queue " << q.id << ": cpus=" << format_cpu_list(q.cpus) << "\n";
    }
    return os.str();
}

} // namespace billet::engine
