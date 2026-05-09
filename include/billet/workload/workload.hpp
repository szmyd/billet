#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace billet::workload {

enum class op_kind : uint8_t {
    read,
    write,
    fsync,
    discard,
    write_zeroes,
};

// Cardinality of op_kind. Update if the enum gains entries; consumers index
// into per-kind arrays via op_kind_index().
inline constexpr size_t k_op_kind_count = static_cast< size_t >(op_kind::write_zeroes) + 1;

// Bounds-checked index for use into per-kind arrays. Eliminates scattered
// static_cast<size_t> sites and gives a stable contract if op_kind ever
// gains a non-dense entry.
constexpr size_t op_kind_index(op_kind k) noexcept {
    auto const i = static_cast< size_t >(k);
    return (i < k_op_kind_count) ? i : 0;
}

// Human/JSON-friendly label for each op_kind. Stable strings -- treated as
// JSON keys in billet.run/1's `by_op` map.
constexpr char const* op_kind_name(op_kind k) noexcept {
    switch (k) {
    case op_kind::read:
        return "Read";
    case op_kind::write:
        return "Write";
    case op_kind::fsync:
        return "Fsync";
    case op_kind::discard:
        return "Discard";
    case op_kind::write_zeroes:
        return "WriteZeroes";
    }
    return "Unknown";
}

// Engine-bound op description. The buffer itself is not carried here -- the
// engine acquires one of `len` bytes from its own per-worker pool at submit
// time.
struct op {
    op_kind  kind{op_kind::read};
    uint16_t component_id{0}; // workload semantics; index into the profile's component table
    uint64_t offset{0};
    uint32_t len{0};
    uint8_t  buf_class{0};
    uint64_t intended_ts_ns{0}; // closed-loop: == issue_ts; open-loop: scheduled time
};

// Per-profile description of one logical (component, op_kind) cell. Stats
// registration, JSON keys, dashboard regexes, and the engine's per-cell
// accounting all derive from a span of these. Adding a new kind to a
// component is a single table edit.
//
//   - json_name appears in by_component JSON keys (e.g. "wal" -> "wal.Fsync")
//   - metric_name appears in the `cell` label under shared metric families
//     -- e.g. billet_component_ops_total{cell="wal.Fsync"}. Decoupled from
//     json_name so a future profile can use display-friendly JSON labels
//     while keeping metric label values underscore-safe; for current
//     profiles they're identical.
struct component_spec {
    std::string_view json_name;
    std::string_view metric_name;
    std::span< op_kind const > kinds;
};

// Flat (component_id, op_kind) -> cell index map derived from a profile's
// component_spec table. -1 entries mark invalid (not-declared-by-spec)
// combinations. Engine, stats::group, and tests all derive identical
// layouts from the same spec via this helper, so cell indices are stable
// across consumers.
struct cell_layout {
    std::vector< std::array< int32_t, k_op_kind_count > > idx;
    size_t cell_count{0};
};

inline cell_layout make_cell_layout(std::span< component_spec const > components) {
    cell_layout out;
    out.idx.resize(components.size());
    for (auto& row : out.idx) { row.fill(-1); }
    for (size_t c = 0; components.size() > c; ++c) {
        for (auto kind : components[c].kinds) {
            out.idx[c][op_kind_index(kind)] = static_cast< int32_t >(out.cell_count++);
        }
    }
    return out;
}

} // namespace billet::workload
