#include <billet/workload/random_read_4k.hpp>

#include <engine/sender_workload_ctx.hpp>
#include <workload/random_read_4k_impl.hpp>

namespace billet::workload {

namespace {
constexpr op_kind        k_reader_kinds[]              = {op_kind::read};
constexpr component_spec k_random_read_4k_components[] = {
    {"reader", "reader", k_reader_kinds},
};
} // namespace

std::span< component_spec const > random_read_4k_components() {
    return std::span< component_spec const >{k_random_read_4k_components};
}

// Production entry point: forward to the templated impl with the engine's
// concrete workload_ctx. The wrapper is a coroutine so callers don't need
// to materialize any stop_token plumbing themselves; one extra coroutine
// frame at startup is the entire cost.
exec::task< void > random_read_4k_run(billet::engine::workload_ctx& ctx, uint64_t device_size_bytes,
                                      uint32_t block_size, uint32_t qd, uint64_t rng_seed) {
    co_await detail::random_read_4k_run_impl(ctx, device_size_bytes, block_size, qd, rng_seed);
    co_return;
}

} // namespace billet::workload
