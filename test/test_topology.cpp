#include <gtest/gtest.h>

#include <vector>

#include <engine/topology.hpp>

namespace {

using namespace billet::engine;

// ---- parse_cpu_list / format_cpu_list -------------------------------------

TEST(parse_cpu_list, single_range) { EXPECT_EQ((std::vector< uint32_t >{0, 1, 2, 3}), parse_cpu_list("0-3")); }

TEST(parse_cpu_list, comma_separated_singletons) {
    EXPECT_EQ((std::vector< uint32_t >{0, 2, 4}), parse_cpu_list("0,2,4"));
}

TEST(parse_cpu_list, mixed_ranges_and_singletons) {
    EXPECT_EQ((std::vector< uint32_t >{0, 1, 4, 5, 8}), parse_cpu_list("0-1,4-5,8"));
}

TEST(parse_cpu_list, sorts_and_dedups) { EXPECT_EQ((std::vector< uint32_t >{1, 2, 3}), parse_cpu_list("3,1,2,1,2")); }

TEST(parse_cpu_list, skips_inverted_and_empty_tokens) {
    EXPECT_EQ((std::vector< uint32_t >{3}), parse_cpu_list(" 3 "));
    EXPECT_TRUE(parse_cpu_list("").empty());
    EXPECT_TRUE(parse_cpu_list("5-3").empty()) << "hi<lo range is skipped, not fatal";
}

TEST(format_cpu_list, round_trips_compact_ranges) {
    EXPECT_EQ("0-3", format_cpu_list({0, 1, 2, 3}));
    EXPECT_EQ("0,2,4", format_cpu_list({0, 2, 4}));
    EXPECT_EQ("0-1,4-5", format_cpu_list({0, 1, 4, 5}));
    EXPECT_EQ("-", format_cpu_list({}));
}

// ---- pin_strategy parse / name --------------------------------------------

TEST(pin_strategy, name_parse_round_trip) {
    for (auto const s :
         {pin_strategy::automatic, pin_strategy::mq, pin_strategy::numa, pin_strategy::linear, pin_strategy::none}) {
        auto const parsed = parse_pin_strategy(pin_strategy_name(s));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(s, *parsed);
    }
}

TEST(pin_strategy, unknown_is_nullopt) { EXPECT_FALSE(parse_pin_strategy("bogus").has_value()); }

// ---- plan_workers ----------------------------------------------------------

namespace {

// Synthetic topology: 8 CPUs, NUMA node 0 owns cpus 0-3. Hardware queues are
// arranged so the NUMA-local queues (those touching cpus 0-3) carry the HIGHER
// ids -- this lets the tests prove local-first ordering is by locality, not id.
device_topology make_topo() {
    device_topology t;
    t.disk = "testdev";
    t.numa_node = 0;
    t.online_cpus = 8;
    t.node_cpus = {0, 1, 2, 3};
    t.queues = {
        {0, {4, 5}}, // remote
        {1, {6, 7}}, // remote
        {2, {0, 1}}, // local
        {3, {2, 3}}, // local
    };
    t.has_mq = true;
    return t;
}

} // namespace

TEST(plan_workers, mq_assigns_local_queues_first) {
    auto const plan = plan_workers(make_topo(), 2, pin_strategy::mq, 0);
    ASSERT_EQ(pin_strategy::mq, plan.strategy);
    ASSERT_EQ(2u, plan.workers.size());
    // Local queues 2 and 3 come before remote 0 and 1.
    EXPECT_EQ((std::vector< uint32_t >{0, 1}), plan.workers[0].cpus);
    EXPECT_EQ(2, plan.workers[0].queue_id);
    EXPECT_EQ((std::vector< uint32_t >{2, 3}), plan.workers[1].cpus);
    EXPECT_EQ(3, plan.workers[1].queue_id);
}

TEST(plan_workers, mq_wraps_when_more_workers_than_queues) {
    auto const plan = plan_workers(make_topo(), 6, pin_strategy::mq, 0);
    ASSERT_EQ(6u, plan.workers.size());
    // Order is [q2, q3, q0, q1]; worker 4 wraps back to q2, worker 5 to q3.
    EXPECT_EQ(2, plan.workers[0].queue_id);
    EXPECT_EQ(1, plan.workers[3].queue_id); // last remote
    EXPECT_EQ(2, plan.workers[4].queue_id);
    EXPECT_EQ(3, plan.workers[5].queue_id);
}

TEST(plan_workers, numa_spreads_single_cpus_round_robin) {
    auto const plan = plan_workers(make_topo(), 6, pin_strategy::numa, 0);
    ASSERT_EQ(pin_strategy::numa, plan.strategy);
    ASSERT_EQ(6u, plan.workers.size());
    EXPECT_EQ((std::vector< uint32_t >{0}), plan.workers[0].cpus);
    EXPECT_EQ((std::vector< uint32_t >{3}), plan.workers[3].cpus);
    EXPECT_EQ((std::vector< uint32_t >{0}), plan.workers[4].cpus) << "wraps over node_cpus";
    EXPECT_EQ(-1, plan.workers[0].queue_id);
}

TEST(plan_workers, linear_uses_base_cpu_modulo_online) {
    auto const plan = plan_workers(make_topo(), 3, pin_strategy::linear, 7);
    ASSERT_EQ(pin_strategy::linear, plan.strategy);
    EXPECT_EQ((std::vector< uint32_t >{7}), plan.workers[0].cpus);
    EXPECT_EQ((std::vector< uint32_t >{0}), plan.workers[1].cpus) << "(7+1) % 8";
    EXPECT_EQ((std::vector< uint32_t >{1}), plan.workers[2].cpus);
}

TEST(plan_workers, none_leaves_workers_unpinned) {
    auto const plan = plan_workers(make_topo(), 3, pin_strategy::none, 0);
    ASSERT_EQ(pin_strategy::none, plan.strategy);
    for (auto const& w : plan.workers) {
        EXPECT_TRUE(w.cpus.empty());
    }
}

TEST(plan_workers, automatic_prefers_mq) {
    auto const plan = plan_workers(make_topo(), 4, pin_strategy::automatic, 0);
    EXPECT_EQ(pin_strategy::mq, plan.strategy);
}

TEST(plan_workers, mq_downgrades_when_no_queues) {
    device_topology t;
    t.disk = "md0";
    t.numa_node = -1;
    t.online_cpus = 4;
    t.node_cpus = {0, 1, 2, 3};
    t.has_mq = false; // stacked device: no hardware queues
    auto const plan = plan_workers(t, 2, pin_strategy::mq, 0);
    EXPECT_EQ(pin_strategy::numa, plan.strategy) << "mq request degrades to numa when no queues exist";
    EXPECT_FALSE(plan.workers[0].cpus.empty());
}

TEST(plan_workers, always_produces_at_least_one_worker) {
    auto const plan = plan_workers(make_topo(), 0, pin_strategy::mq, 0);
    EXPECT_EQ(1u, plan.workers.size());
}

// ---- auto_worker_count -----------------------------------------------------

TEST(auto_worker_count, counts_numa_local_queues) {
    // Two of the four queues are local to node 0.
    EXPECT_EQ(2u, auto_worker_count(make_topo()));
}

TEST(auto_worker_count, no_numa_affinity_counts_all_queues) {
    auto t = make_topo();
    t.numa_node = -1; // every queue is equally local
    EXPECT_EQ(4u, auto_worker_count(t));
}

TEST(auto_worker_count, falls_back_to_node_cpu_count_without_mq) {
    device_topology t;
    t.numa_node = 0;
    t.online_cpus = 8;
    t.node_cpus = {0, 1, 2, 3};
    t.has_mq = false;
    EXPECT_EQ(4u, auto_worker_count(t));
}

TEST(auto_worker_count, capped_at_online_cpus) {
    auto t = make_topo();
    t.numa_node = -1;
    t.online_cpus = 3; // fewer cores than queues
    EXPECT_EQ(3u, auto_worker_count(t));
}

// ---- discover_topology degradation ----------------------------------------

TEST(discover_topology, degrades_for_nonexistent_device) {
    // No /sys/class/block entry: no mq, but the global online mask still yields
    // a usable CPU count so placement can fall back to linear.
    auto const topo = discover_topology("/dev/billet-does-not-exist");
    EXPECT_FALSE(topo.has_mq);
    EXPECT_GE(topo.online_cpus, 1u);
}

} // namespace
