#pragma once

#include <cstdint>
#include <span>

#include <exec/task.hpp>

#include <billet/workload/workload.hpp>

namespace billet::engine {
class workload_ctx;
} // namespace billet::engine

namespace billet::workload::profiles {

namespace pgwc_component {
constexpr uint16_t commit_write = 0;
constexpr uint16_t commit_fsync = 1;
} // namespace pgwc_component

std::span< component_spec const > pg_wal_commit_components();

// Models hundreds of PostgreSQL sessions each doing synchronous WAL commit:
// write(8 KiB) + fdatasync, repeated in a tight closed loop, with no read
// traffic and no group-commit batching. Set --qd to the session count.
struct pg_wal_commit_config {
    uint64_t device_size_bytes{0};
    uint32_t write_size_bytes{8192};                // 8 KiB per write+fsync
    uint64_t region_per_session_bytes{64ull << 20}; // 64 MiB cycling window per session
};

exec::task< void > pg_wal_commit_run(billet::engine::workload_ctx& ctx, pg_wal_commit_config const& cfg,
                                      uint32_t sessions);

} // namespace billet::workload::profiles
