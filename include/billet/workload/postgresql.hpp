#pragma once

#include <cstdint>
#include <span>

#include <exec/task.hpp>

#include <billet/workload/workload.hpp>

namespace billet::engine {
class workload_ctx;
} // namespace billet::engine

namespace billet::workload::profiles {

// Component IDs for the PostgreSQL profile. Indices into the spec table
// returned by postgresql_components(). Each emitter constructor stamps the
// corresponding constant onto every op it produces.
namespace pg_component {
constexpr uint16_t reader      = 0;
constexpr uint16_t rand_writer = 1;
constexpr uint16_t wal         = 2; // emits both Write and Fsync
constexpr uint16_t ckpt        = 3;
} // namespace pg_component

// Component table for the PostgreSQL profile -- the single source of truth
// for which (component, op_kind) cells stats::group registers, JSON output
// uses as keys, and the engine accounts per-cell.
std::span< component_spec const > postgresql_components();

// PostgreSQL-shaped profile config. See docs/profiles/postgresql.md for the
// workload semantics + WAL drain discipline.
struct postgresql_config {
    uint64_t device_size_bytes{0};

    uint32_t readers{4};
    uint32_t writers{2};

    uint64_t wal_region_offset{0};
    uint64_t wal_region_size{1ull << 30}; // 1 GiB cyclic WAL region
    uint64_t wal_bytes_per_sec{50ull << 20};
    uint32_t wal_fsync_every_ms{200};
    uint32_t wal_fsync_every_writes{0};

    uint32_t ckpt_period_ms{5000};
    uint64_t ckpt_burst_bytes{256ull << 20}; // 256 MiB burst
    uint32_t ckpt_block_size{65536};         // 64 KiB writes

    uint32_t reader_target_iops{2000};
    double   hot_set_frac{0.10};
    double   locality{0.85};

    uint32_t writer_target_iops{500};

    uint32_t block_size_8k{8192};

    uint64_t rng_seed{0}; // 0 -> seed from std::random_device
};

// Production entry point. Spawns reader / rand_writer / wal / checkpointer
// emitter coroutines into an internal async_scope; returns when ctx.stopped()
// flips and all emitters drain. Implementation is templated on Ctx in
// src/workload/postgresql_impl.hpp; only the engine::workload_ctx
// instantiation is exposed here.
exec::task< void > postgresql_run(billet::engine::workload_ctx& ctx, postgresql_config const& cfg, uint32_t qd);

} // namespace billet::workload::profiles
