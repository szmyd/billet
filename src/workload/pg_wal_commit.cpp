#include <billet/workload/pg_wal_commit.hpp>

#include <engine/sender_workload_ctx.hpp>
#include <workload/pg_wal_commit_impl.hpp>

namespace billet::workload::profiles {

namespace {
constexpr op_kind k_commit_write_kinds[] = {op_kind::write};
constexpr op_kind k_commit_fsync_kinds[] = {op_kind::fsync};

constexpr component_spec k_pgwc_components[] = {
    {"commit_write", "commit_write", k_commit_write_kinds},
    {"commit_fsync", "commit_fsync", k_commit_fsync_kinds},
};
} // namespace

std::span< component_spec const > pg_wal_commit_components() {
    return std::span< component_spec const >{k_pgwc_components};
}

exec::task< void > pg_wal_commit_run(billet::engine::workload_ctx& ctx, pg_wal_commit_config const& cfg,
                                      uint32_t sessions) {
    co_await detail::pg_wal_commit_run_impl(ctx, cfg, sessions);
    co_return;
}

} // namespace billet::workload::profiles
