#include <gtest/gtest.h>

#include <billet/workload/workload.hpp>
#include <billet/workload/postgresql.hpp>

namespace {

using billet::workload::op_kind;
using billet::workload::profiles::postgresql_components;
namespace pg_component = billet::workload::profiles::pg_component;

// The legacy callback-workload pg_wal drain tests retired with the
// callback engine in Phase 4. Behavioral coverage of the sender pg_wal
// drain semantics will land alongside the workload_ctx templating pass
// + manual_scheduler-based test ctx (Phase 4c).
TEST(postgresql_components, declares_expected_cells) {
    auto const specs = postgresql_components();
    ASSERT_EQ(4u, specs.size());

    EXPECT_EQ("reader",      specs[pg_component::reader].json_name);
    EXPECT_EQ("rand_writer", specs[pg_component::rand_writer].json_name);
    EXPECT_EQ("wal",         specs[pg_component::wal].json_name);
    EXPECT_EQ("ckpt",        specs[pg_component::ckpt].json_name);

    ASSERT_EQ(1u, specs[pg_component::reader].kinds.size());
    EXPECT_EQ(op_kind::read, specs[pg_component::reader].kinds[0]);

    ASSERT_EQ(1u, specs[pg_component::rand_writer].kinds.size());
    EXPECT_EQ(op_kind::write, specs[pg_component::rand_writer].kinds[0]);

    ASSERT_EQ(2u, specs[pg_component::wal].kinds.size());
    EXPECT_EQ(op_kind::write, specs[pg_component::wal].kinds[0]);
    EXPECT_EQ(op_kind::fsync, specs[pg_component::wal].kinds[1]);

    ASSERT_EQ(1u, specs[pg_component::ckpt].kinds.size());
    EXPECT_EQ(op_kind::write, specs[pg_component::ckpt].kinds[0]);
}

} // namespace
