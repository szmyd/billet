#include <gtest/gtest.h>

#include <cstdint>

#include <billet/workload/workload.hpp>
#include <workload/scheduling.hpp>

namespace {

using namespace billet::workload;

TEST(op_kind, names_match_schema_keys) {
    EXPECT_STREQ("Read",        op_kind_name(op_kind::read));
    EXPECT_STREQ("Write",       op_kind_name(op_kind::write));
    EXPECT_STREQ("Fsync",       op_kind_name(op_kind::fsync));
    EXPECT_STREQ("Discard",     op_kind_name(op_kind::discard));
    EXPECT_STREQ("WriteZeroes", op_kind_name(op_kind::write_zeroes));
}

TEST(op_kind, index_is_dense_and_in_bounds) {
    EXPECT_EQ(0u, op_kind_index(op_kind::read));
    EXPECT_EQ(1u, op_kind_index(op_kind::write));
    EXPECT_EQ(2u, op_kind_index(op_kind::fsync));
    EXPECT_EQ(3u, op_kind_index(op_kind::discard));
    EXPECT_EQ(4u, op_kind_index(op_kind::write_zeroes));
    EXPECT_EQ(5u, k_op_kind_count);
}

TEST(component_spec, cell_layout_dense_and_sparse_for_pg_shape) {
    // A 4-component spec with mixed kind counts (matches PG's shape: reader=read,
    // rand_writer=write, wal=write+fsync, ckpt=write). cell_count must equal the
    // sum of kinds across components, and cell_idx values must be dense [0,N).
    static constexpr op_kind reader_kinds[]      = {op_kind::read};
    static constexpr op_kind rand_writer_kinds[] = {op_kind::write};
    static constexpr op_kind wal_kinds[]         = {op_kind::write, op_kind::fsync};
    static constexpr op_kind ckpt_kinds[]        = {op_kind::write};
    static constexpr component_spec specs[] = {
        {"reader",      "reader",      reader_kinds},
        {"rand_writer", "rand_writer", rand_writer_kinds},
        {"wal",         "wal",         wal_kinds},
        {"ckpt",        "ckpt",        ckpt_kinds},
    };
    auto const layout = make_cell_layout(specs);
    EXPECT_EQ(5u, layout.cell_count);
    EXPECT_EQ(4u, layout.idx.size());

    // Declared cells: dense indices 0..4.
    EXPECT_EQ(0, layout.idx[0][op_kind_index(op_kind::read)]);
    EXPECT_EQ(1, layout.idx[1][op_kind_index(op_kind::write)]);
    EXPECT_EQ(2, layout.idx[2][op_kind_index(op_kind::write)]);
    EXPECT_EQ(3, layout.idx[2][op_kind_index(op_kind::fsync)]);
    EXPECT_EQ(4, layout.idx[3][op_kind_index(op_kind::write)]);

    // Undeclared cells (e.g. reader.Fsync, ckpt.Read): -1.
    EXPECT_EQ(-1, layout.idx[0][op_kind_index(op_kind::fsync)]);
    EXPECT_EQ(-1, layout.idx[0][op_kind_index(op_kind::write)]);
    EXPECT_EQ(-1, layout.idx[3][op_kind_index(op_kind::read)]);
    EXPECT_EQ(-1, layout.idx[2][op_kind_index(op_kind::discard)]);
}

TEST(component_spec, empty_spec_yields_empty_layout) {
    auto const layout = make_cell_layout({});
    EXPECT_EQ(0u, layout.cell_count);
    EXPECT_TRUE(layout.idx.empty());
}

TEST(poisson_emitter, mean_inter_arrival_matches_target_rate) {
    constexpr double k_rate    = 1000.0;       // ops/sec
    constexpr int    k_samples = 5000;

    poisson_emitter emit(k_rate, /*seed=*/42);

    uint64_t now      = 0;
    uint64_t prev_ts  = 0;
    bool     have_prev = false;
    double   sum_ns   = 0.0;
    int      count    = 0;

    while (count < k_samples) {
        uint64_t ts = 0;
        if (emit.try_advance(now, ts)) {
            if (have_prev) {
                sum_ns += static_cast< double >(ts - prev_ts);
            }
            prev_ts   = ts;
            have_prev = true;
            ++count;
        } else {
            now = emit.deadline_ns();
        }
    }

    double const observed_mean_ns = sum_ns / static_cast< double >(count - 1);
    double const expected_mean_ns = 1e9 / k_rate;
    // Poisson sample variance shrinks like sqrt(N); 5% tolerance is generous.
    EXPECT_NEAR(expected_mean_ns, observed_mean_ns, expected_mean_ns * 0.05)
        << "observed mean " << observed_mean_ns << " ns vs expected " << expected_mean_ns;
}

TEST(poisson_emitter, expected_interval_matches_rate) {
    poisson_emitter emit(2000.0, /*seed=*/0);
    EXPECT_EQ(uint64_t{500'000}, emit.expected_interval_ns()); // 1e9 / 2000
}

} // namespace
