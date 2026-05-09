#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

// Shared HDR ownership + factory for the engine. Centralizes the lowest /
// highest / sigfig configuration so every histogram billet allocates uses
// identical bucket geometry; mismatches would prevent merging via hdr_add.
//
// hdr_histogram stays an incomplete type for callers; only hdr.cpp pulls in
// <hdr/hdr_histogram.h>.

struct hdr_histogram;

namespace billet::engine {

// Stateless functor deleter: hdr_deleter is default-constructible, which lets
// std::array<hdr_ptr, N>{} default-init cleanly. (A function-pointer deleter
// would not -- unique_ptr's default ctor is ill-formed when the deleter is a
// pointer type per [unique.ptr.single.ctor]/2.)
struct hdr_deleter {
    void operator()(hdr_histogram* h) const noexcept;
};

using hdr_ptr = std::unique_ptr< hdr_histogram, hdr_deleter >;

// Range/precision constants applied to every HDR billet allocates.
inline constexpr int64_t k_hdr_lowest_ns = 1;
inline constexpr int64_t k_hdr_highest_ns =
    std::chrono::nanoseconds{std::chrono::seconds{60}}.count();
inline constexpr int k_hdr_sigfigs = 3;

// Allocates and initializes an empty histogram. Returns an empty hdr_ptr on
// hdr_init failure.
hdr_ptr make_hdr();

} // namespace billet::engine
