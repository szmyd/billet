#pragma once

#include <cstdint>
#include <span>

#include <exec/task.hpp>

#include <billet/workload/workload.hpp>

namespace billet::engine {
class workload_ctx;
} // namespace billet::engine

namespace billet::workload {

// Single-component spec used by the random_read_4k profile.
std::span< component_spec const > random_read_4k_components();

// Production entry point. Spawns `qd` parallel reader coroutines, each
// looping `co_await ctx.submit_op(read)` until ctx.stopped(). Closed-loop:
// each loop iteration awaits its op, so the natural concurrency limit is
// the number of spawned coroutines. Implementation is templated on Ctx in
// src/workload/random_read_4k_impl.hpp; only the engine::workload_ctx
// instantiation is exposed here.
exec::task< void > random_read_4k_run(billet::engine::workload_ctx& ctx, uint64_t device_size_bytes,
                                      uint32_t block_size, uint32_t qd, uint64_t rng_seed);

} // namespace billet::workload
