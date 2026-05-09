#pragma once

// Templated implementation of random_read_4k_run. Included by
// src/workload/random_read_4k.cpp (which explicitly instantiates against
// engine::workload_ctx) and by tests that wish to instantiate against an
// alternative ctx (e.g. a manual_scheduler-backed test_ctx). External
// profile authors should NOT include this header -- it is private to
// billet's own profile implementations.

#include <random>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

#include <billet/workload/random_read_4k.hpp>

namespace billet::workload::detail {

// One closed-loop reader: pulls offsets from a per-coroutine RNG, submits
// 4K reads, awaits each completion, loops until the deadline flips
// ctx.stopped() to true.
template < typename Ctx >
exec::task< void > random_read_4k_reader_loop(Ctx& ctx, uint64_t device_size_bytes, uint32_t block_size,
                                              uint64_t seed) {
    std::mt19937_64                           rng(0 == seed ? std::random_device{}() : seed);
    uint64_t const                            n_blocks = device_size_bytes / block_size;
    std::uniform_int_distribution< uint64_t > dist(0, (0 < n_blocks) ? n_blocks - 1 : 0);

    while (!ctx.stopped()) {
        op o{};
        o.kind           = op_kind::read;
        o.component_id   = 0;
        o.offset         = dist(rng) * block_size;
        o.len            = block_size;
        o.intended_ts_ns = ctx.now_ns();
        // Discard the completion struct -- the engine's accumulator inside
        // submit_op already recorded the stats; reader_loop only cares
        // that the op finished so it can issue the next.
        (void)co_await ctx.submit_op(o);
    }
    co_return;
}

template < typename Ctx >
exec::task< void > random_read_4k_run_impl(Ctx& ctx, uint64_t device_size_bytes, uint32_t block_size, uint32_t qd,
                                           uint64_t rng_seed) {
    exec::async_scope inner;
    for (uint32_t i = 0; qd > i; ++i) {
        // Each reader gets its own seed so their offset sequences are
        // independent across the parallel coroutines. With rng_seed=0
        // every reader pulls from std::random_device individually.
        uint64_t const child_seed = (0 == rng_seed) ? 0ULL : (rng_seed ^ (uint64_t{i} * 0x9E3779B97F4A7C15ULL));
        inner.spawn(random_read_4k_reader_loop(ctx, device_size_bytes, block_size, child_seed));
    }
    // when_empty() in this stdexec build requires an inner sender that
    // completes after the scope drains; pass stdexec::just() so the
    // sender completes immediately once the scope is empty.
    co_await inner.when_empty(stdexec::just());
    co_return;
}

} // namespace billet::workload::detail
