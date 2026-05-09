#include <billet/workload/postgresql.hpp>

#include <engine/sender_workload_ctx.hpp>
#include <workload/postgresql_impl.hpp>

namespace billet::workload::profiles {

namespace {
constexpr op_kind k_reader_kinds[]      = {op_kind::read};
constexpr op_kind k_rand_writer_kinds[] = {op_kind::write};
constexpr op_kind k_wal_kinds[]         = {op_kind::write, op_kind::fsync};
constexpr op_kind k_ckpt_kinds[]        = {op_kind::write};

constexpr component_spec k_pg_components[] = {
    {"reader",      "reader",      k_reader_kinds},
    {"rand_writer", "rand_writer", k_rand_writer_kinds},
    {"wal",         "wal",         k_wal_kinds},
    {"ckpt",        "ckpt",        k_ckpt_kinds},
};
} // namespace

std::span< component_spec const > postgresql_components() {
    return std::span< component_spec const >{k_pg_components};
}

// Production entry point: forward to the templated impl with the engine's
// concrete workload_ctx. The wrapper is a coroutine so callers don't need
// to materialize any stop_token plumbing themselves; one extra coroutine
// frame at startup is the entire cost.
exec::task< void > postgresql_run(billet::engine::workload_ctx& ctx, postgresql_config const& cfg, uint32_t qd) {
    co_await detail::postgresql_run_impl(ctx, cfg, qd);
    co_return;
}

} // namespace billet::workload::profiles
