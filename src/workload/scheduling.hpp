#pragma once

#include <algorithm>
#include <cstdint>
#include <random>

// Workload-internal scheduling and RNG primitives. Header-only; pulled in by
// any profile implementation that needs open-loop Poisson arrivals or seeded
// PRNG state. Lives in src/workload/ rather than include/billet/workload/
// because consumers are workload implementations, not external API users.

namespace billet::workload {

// Seed an mt19937_64 from `seed`, or from std::random_device if seed == 0.
inline std::mt19937_64 make_rng(uint64_t seed) {
    if (0 != seed) { return std::mt19937_64{seed}; }
    std::random_device rd;
    return std::mt19937_64{(static_cast< uint64_t >(rd()) << 32) ^ static_cast< uint64_t >(rd())};
}

// Open-loop Poisson scheduler. Holds the next scheduled arrival timestamp;
// each successful try_advance() consumes the cursor and advances by an
// exponentially-distributed inter-arrival sized to the target rate.
//
// Used by any workload that needs to fire ops at a target rate with
// CO-correct intended_ts_ns: PostgreSQL readers/writers/WAL today, expect
// MongoDB / Kafka / Elasticsearch to share this when they land.
class poisson_emitter {
public:
    poisson_emitter(double rate_per_sec, uint64_t seed) : _rng(make_rng(seed)), _dist(std::max(1.0, rate_per_sec)) {
        _expected_interval_ns = static_cast< uint64_t >(1e9 / std::max(1.0, rate_per_sec));
    }

    // If the next scheduled timestamp is at or before now_ns, sets out_ts to
    // that timestamp, advances the cursor by an Exp inter-arrival, and
    // returns true. Otherwise returns false (caller should poll later).
    bool try_advance(uint64_t now_ns, uint64_t& out_ts) {
        if (0 == _next_ts) { _next_ts = now_ns; }
        if (_next_ts > now_ns) { return false; }
        out_ts = _next_ts;
        double const inter_ns = _dist(_rng) * 1e9;
        _next_ts += static_cast< uint64_t >(inter_ns);
        return true;
    }

    uint64_t deadline_ns() const noexcept { return _next_ts; }
    uint64_t expected_interval_ns() const noexcept { return _expected_interval_ns; }

private:
    std::mt19937_64 _rng;
    std::exponential_distribution<> _dist;
    uint64_t _next_ts{0};
    uint64_t _expected_interval_ns;
};

} // namespace billet::workload
