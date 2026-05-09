#include <engine/hdr.hpp>

#include <hdr/hdr_histogram.h>

namespace billet::engine {

void hdr_deleter::operator()(hdr_histogram* h) const noexcept {
    if (nullptr != h) { ::hdr_close(h); }
}

hdr_ptr make_hdr() {
    hdr_histogram* raw = nullptr;
    if (0 != ::hdr_init(k_hdr_lowest_ns, k_hdr_highest_ns, k_hdr_sigfigs, &raw)) { return hdr_ptr{}; }
    return hdr_ptr{raw};
}

} // namespace billet::engine
