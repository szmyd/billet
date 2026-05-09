#include <gtest/gtest.h>

#include <chrono>
#include <regex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <report/json_writer.hpp>
#include <report/run_record.hpp>

namespace {

using billet::report::iso8601_now;
using billet::report::make_ulid;
using billet::report::op_stats;
using billet::report::run_record;
using billet::report::to_json_string;

TEST(make_ulid, length_and_alphabet) {
    auto const u = make_ulid();
    ASSERT_EQ(26u, u.size());
    static constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (char c : u) {
        EXPECT_NE(std::string_view::npos, alphabet.find(c)) << "non-Crockford-base32 char '" << c << "'";
    }
}

TEST(make_ulid, lexicographically_sortable_by_time) {
    auto const a = make_ulid();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto const b = make_ulid();
    EXPECT_LT(a, b) << "ULIDs minted later must sort after earlier ones";
}

TEST(iso8601_now, matches_utc_pattern) {
    auto const s = iso8601_now();
    static std::regex const re{R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)"};
    EXPECT_TRUE(std::regex_match(s, re)) << "got: " << s;
}

TEST(to_json_string, key_fields_round_trip) {
    run_record rec;
    rec.schema_version = "billet.run/1";
    rec.run_id         = "01TEST00000000000000000000";
    rec.started_at     = "2026-05-09T20:00:00Z";
    rec.duration_s     = 5.0;

    rec.host.hostname = "test-host";
    rec.host.kernel   = "6.18.22-test";
    rec.host.cores    = 8;
    rec.host.ram_gb   = 32.0;

    rec.device.path                   = "/dev/loop0";
    rec.device.size_bytes             = uint64_t{4} << 30;
    rec.device.logical_block          = 4096;
    rec.device.physical_block         = 4096;
    rec.device.rotational             = false;
    rec.device.discard_supported      = true;
    rec.device.write_zeroes_supported = false;
    rec.device.label                  = "ut";

    rec.profile.name              = "random_read_4k";
    rec.profile.version           = "1";
    rec.profile.params["block_size"] = "4096";
    rec.profile.params["workers"]    = "2";

    rec.engine.name          = "io_uring";
    rec.engine.qd_per_worker = 32;
    rec.engine.workers       = 2;
    rec.engine.o_direct      = true;

    rec.results.summary.ops_total        = 1000;
    rec.results.summary.bytes_total      = 1000ull * 4096;
    rec.results.summary.iops_mean        = 200.0;
    rec.results.summary.throughput_mibs  = 0.78;
    rec.results.summary.errors           = 0;
    rec.results.summary.component_drops  = 7;

    op_stats os{};
    os.count    = 1000;
    os.bytes    = 1000ull * 4096;
    os.p50_us   = 100;
    os.p99_us   = 250;
    os.p99_9_us = 700;
    os.max_us   = 2000;
    os.hdr_b64  = "stub-hdr-b64";
    rec.results.by_op["Read"] = os;

    // by_component breaks the same op out by component label so the JSON
    // surfaces both views: "wal.Fsync" carries WAL-specific fsync stats,
    // distinct from the kind-aggregate "Fsync" line in by_op.
    op_stats wal_fsync{};
    wal_fsync.count   = 50;
    wal_fsync.p50_us  = 800;
    wal_fsync.p99_us  = 4500;
    wal_fsync.max_us  = 9000;
    wal_fsync.hdr_b64 = "stub-wal-fsync-b64";
    rec.results.by_component["wal.Fsync"] = wal_fsync;

    auto const  body   = to_json_string(rec);
    auto const  parsed = nlohmann::json::parse(body);

    EXPECT_EQ("billet.run/1",                parsed["schema_version"].get< std::string >());
    EXPECT_EQ("01TEST00000000000000000000",  parsed["run_id"].get< std::string >());
    EXPECT_DOUBLE_EQ(5.0,                    parsed["duration_s"].get< double >());
    EXPECT_EQ("test-host",                   parsed["host"]["hostname"].get< std::string >());
    EXPECT_EQ(8u,                            parsed["host"]["cores"].get< uint32_t >());
    EXPECT_EQ("/dev/loop0",                  parsed["device"]["path"].get< std::string >());
    EXPECT_EQ(4096u,                         parsed["device"]["physical_block"].get< uint32_t >());
    EXPECT_EQ(false,                         parsed["device"]["rotational"].get< bool >());
    EXPECT_EQ(true,                          parsed["device"]["discard_supported"].get< bool >());
    EXPECT_EQ("random_read_4k",              parsed["profile"]["name"].get< std::string >());
    EXPECT_EQ("4096",                        parsed["profile"]["params"]["block_size"].get< std::string >());
    EXPECT_EQ("2",                           parsed["profile"]["params"]["workers"].get< std::string >());
    EXPECT_EQ("io_uring",                    parsed["engine"]["name"].get< std::string >());
    EXPECT_EQ(32u,                           parsed["engine"]["qd_per_worker"].get< uint32_t >());
    EXPECT_EQ(2u,                            parsed["engine"]["workers"].get< uint32_t >());
    EXPECT_EQ(1000u,                         parsed["results"]["summary"]["ops_total"].get< uint64_t >());
    EXPECT_EQ(7u,                            parsed["results"]["summary"]["component_drops"].get< uint64_t >());
    EXPECT_EQ(1000u,                         parsed["results"]["by_op"]["Read"]["count"].get< uint64_t >());
    EXPECT_EQ(250,                           parsed["results"]["by_op"]["Read"]["p99_us"].get< int64_t >());
    EXPECT_EQ("stub-hdr-b64",                parsed["results"]["by_op"]["Read"]["hdr_b64"].get< std::string >());

    ASSERT_TRUE(parsed["results"]["by_component"].is_object());
    EXPECT_EQ(50u,    parsed["results"]["by_component"]["wal.Fsync"]["count"].get< uint64_t >());
    EXPECT_EQ(4500,   parsed["results"]["by_component"]["wal.Fsync"]["p99_us"].get< int64_t >());
    EXPECT_EQ("stub-wal-fsync-b64",
              parsed["results"]["by_component"]["wal.Fsync"]["hdr_b64"].get< std::string >());
}

TEST(to_json_string, empty_by_op_serializes_as_empty_object) {
    run_record rec;
    rec.schema_version = "billet.run/1";
    auto const body    = to_json_string(rec);
    auto const parsed  = nlohmann::json::parse(body);
    EXPECT_TRUE(parsed["results"]["by_op"].is_object());
    EXPECT_TRUE(parsed["results"]["by_op"].empty());
}

} // namespace
