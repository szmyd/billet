#include <gtest/gtest.h>

#include <hdr/hdr_histogram.h>

#include <cstdint>
#include <stdexcept>

#include <engine/aligned_buf.hpp>
#include <engine/hdr.hpp>

namespace {

using billet::engine::aligned_buf_pool;
using billet::engine::make_hdr;

TEST(aligned_buf_pool, acquire_returns_aligned_pointer) {
    aligned_buf_pool pool(4096);
    auto             buf = pool.acquire(4096);
    ASSERT_TRUE(static_cast< bool >(buf));
    EXPECT_EQ(0u, reinterpret_cast< uintptr_t >(buf.data()) % 4096);
    EXPECT_EQ(4096u, buf.size());
}

TEST(aligned_buf_pool, acquire_rounds_up_to_size_class) {
    aligned_buf_pool pool(4096);
    auto             small = pool.acquire(1500);
    EXPECT_EQ(4 * 1024u, small.size()) << "1500 should round up to the 4K class";
    auto medium = pool.acquire(5000);
    EXPECT_EQ(8 * 1024u, medium.size()) << "5000 should round up to the 8K class";
    auto large = pool.acquire(60 * 1024);
    EXPECT_EQ(64 * 1024u, large.size()) << "60K should round up to the 64K class";
}

TEST(aligned_buf_pool, freelist_reuses_released_buffer) {
    aligned_buf_pool pool(4096);
    void* first_addr = nullptr;
    {
        auto buf  = pool.acquire(4096);
        first_addr = buf.data();
    }
    auto reused = pool.acquire(4096);
    EXPECT_EQ(first_addr, reused.data()) << "released buffer should come back from freelist";
}

TEST(aligned_buf_pool, alignment_must_be_power_of_two) {
    EXPECT_THROW(aligned_buf_pool{3000}, std::invalid_argument);
}

TEST(aligned_buf_pool, alignment_must_be_at_least_pointer_sized) {
    EXPECT_THROW(aligned_buf_pool{4}, std::invalid_argument);
}

TEST(aligned_buf_pool, oversize_throws_bad_alloc) {
    aligned_buf_pool pool(4096);
    // The largest size class is 512 KiB; anything larger is out of range.
    EXPECT_THROW(pool.acquire(2 * 1024 * 1024), std::bad_alloc);
}

TEST(aligned_buf_pool, alignment_holds_for_128k_physical) {
    // Mirrors a virtual device advertising a 128 KiB physical block size.
    aligned_buf_pool pool(128 * 1024);
    auto             buf = pool.acquire(8192);
    EXPECT_EQ(0u, reinterpret_cast< uintptr_t >(buf.data()) % (128 * 1024))
        << "physical block size alignment must hold even for sub-class allocations";
}

TEST(hdr, make_hdr_returns_valid_histogram) {
    auto h = make_hdr();
    ASSERT_TRUE(static_cast< bool >(h));
    EXPECT_EQ(0, h->total_count) << "fresh histogram should have zero recorded samples";
}

TEST(hdr, record_value_persists) {
    auto h = make_hdr();
    ::hdr_record_value(h.get(), 100'000); // 100us
    ::hdr_record_value(h.get(), 200'000); // 200us
    ::hdr_record_value(h.get(), 300'000); // 300us
    EXPECT_EQ(3, h->total_count);
    EXPECT_NEAR(200'000, ::hdr_value_at_percentile(h.get(), 50.0), 1000);
    EXPECT_NEAR(300'000, ::hdr_max(h.get()), 1000);
}

TEST(hdr, hdr_add_merges_counts_and_distribution) {
    auto a = make_hdr();
    auto b = make_hdr();
    for (int i = 0; 100 > i; ++i) { ::hdr_record_value(a.get(), 50'000 + i); }
    for (int i = 0; 200 > i; ++i) { ::hdr_record_value(b.get(), 500'000 + i); }

    auto merged = make_hdr();
    ::hdr_add(merged.get(), a.get());
    ::hdr_add(merged.get(), b.get());

    EXPECT_EQ(300, merged->total_count);
    // Median of {100 samples around 50us, 200 samples around 500us} sits in
    // the 500us bucket since it dominates by count.
    EXPECT_NEAR(500'000, ::hdr_value_at_percentile(merged.get(), 50.0), 1000);
    // Min comes from the smaller-value source.
    EXPECT_NEAR(50'000, ::hdr_min(merged.get()), 1000);
}

} // namespace
