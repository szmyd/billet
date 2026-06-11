#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include <boost/preprocessor/stringize.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>

#include <cli/progress.hpp>
#include <engine/device_probe.hpp>
#include <engine/ring_worker.hpp>
#include <engine/sender_engine.hpp>
#include <report/json_writer.hpp>
#include <report/run_record.hpp>
#include <stats/stats.hpp>
#include <billet/workload/pg_wal_commit.hpp>
#include <billet/workload/postgresql.hpp>
#include <billet/workload/random_read_4k.hpp>

SISL_OPTION_GROUP(
    billet,
    (probe, "", "probe", "Probe a block device and print its geometry, then exit", ::cxxopts::value< std::string >(),
     "<path>"),
    (device, "d", "device", "Block device to benchmark", ::cxxopts::value< std::string >(), "<path>"),
    (profile, "p", "profile", "Workload profile (random_read_4k | postgresql | pg_wal_commit)",
     ::cxxopts::value< std::string >()->default_value("random_read_4k"), "<name>"),
    (workers, "w", "workers", "Number of pinned worker threads", ::cxxopts::value< uint32_t >()->default_value("1"),
     ""),
    (duration, "t", "duration", "Run duration in seconds", ::cxxopts::value< uint32_t >()->default_value("30"), ""),
    (qd, "", "qd", "Queue depth per worker (closed-loop)", ::cxxopts::value< uint32_t >()->default_value("32"), ""),
    (allow_destructive, "", "allow-destructive",
     "Approve destructive ops (Write/Discard/Fsync/WriteZeroes) non-interactively. "
     "Without this flag, billet prompts before running a destructive profile against "
     "the device; absent both the flag and a TTY (e.g. CI), the run is refused.",
     ::cxxopts::value< bool >()->default_value("false"), ""),
    (output, "o", "output", "Write per-run JSON (billet.run/1) to <path>", ::cxxopts::value< std::string >(), "<path>"),
    (label, "", "device-label", "Free-form device label recorded in JSON output",
     ::cxxopts::value< std::string >()->default_value(""), ""),
    (metrics_port, "", "metrics-port",
     "TCP port to expose Prometheus /metrics on. 0 disables. See example/grafana/ "
     "for a docker-compose dashboard stack.",
     ::cxxopts::value< uint16_t >()->default_value("0"), "<port>"),
    (metrics_drain_s, "", "metrics-drain-s",
     "Seconds to keep /metrics up after the run so a final Prometheus scrape "
     "catches terminal counters before tear-down. 0 disables.",
     ::cxxopts::value< uint32_t >()->default_value("30"), ""),
    (pg_readers, "", "pg-readers", "(postgresql) Number of reader emitters",
     ::cxxopts::value< uint32_t >()->default_value("4"), ""),
    (pg_reader_iops, "", "pg-reader-iops", "(postgresql) Per-reader open-loop target IOPS",
     ::cxxopts::value< uint32_t >()->default_value("2000"), ""),
    (pg_writers, "", "pg-writers", "(postgresql) Number of random-writer emitters",
     ::cxxopts::value< uint32_t >()->default_value("2"), ""),
    (pg_writer_iops, "", "pg-writer-iops", "(postgresql) Per-writer open-loop target IOPS",
     ::cxxopts::value< uint32_t >()->default_value("500"), ""),
    (pg_wal_mb_per_sec, "", "pg-wal-mb-per-sec", "(postgresql) WAL writer target throughput in MiB/s",
     ::cxxopts::value< uint32_t >()->default_value("50"), ""),
    (pg_wal_fsync_ms, "", "pg-wal-fsync-ms", "(postgresql) Periodic Fsync interval (ms); 0 disables time-based fsync",
     ::cxxopts::value< uint32_t >()->default_value("200"), ""),
    (pg_ckpt_period_ms, "", "pg-ckpt-period-ms", "(postgresql) Checkpoint burst period (ms)",
     ::cxxopts::value< uint32_t >()->default_value("5000"), ""),
    (pg_ckpt_burst_mb, "", "pg-ckpt-burst-mb", "(postgresql) Bytes flushed per checkpoint burst (MiB)",
     ::cxxopts::value< uint32_t >()->default_value("256"), ""),
    (pg_hot_set_frac, "", "pg-hot-set-frac", "(postgresql) Fraction of device that is the read hot set",
     ::cxxopts::value< double >()->default_value("0.10"), ""),
    (pg_locality, "", "pg-locality", "(postgresql) Probability of a read targeting the hot set",
     ::cxxopts::value< double >()->default_value("0.85"), ""),
    (pgwc_write_size, "", "pgwc-write-size", "(pg_wal_commit) Write size per commit in bytes",
     ::cxxopts::value< uint32_t >()->default_value("8192"), ""),
    (pgwc_region_mb, "", "pgwc-region-mb", "(pg_wal_commit) Per-session cycling region in MiB",
     ::cxxopts::value< uint32_t >()->default_value("64"), ""))

#define ENABLED_OPTIONS logging, billet

SISL_OPTIONS_ENABLE(ENABLED_OPTIONS)

SISL_LOGGING_INIT()

namespace {

struct profile_descriptor {
    std::string_view name;
    bool destructive;
    uint32_t pool_block_size;
};

profile_descriptor const* lookup_profile(std::string_view name) {
    static constexpr profile_descriptor table[] = {
        {"random_read_4k", false, 4096},
        {"postgresql", true, 65536},
        {"pg_wal_commit", true, 65536},
    };
    for (auto const& p : table) {
        if (p.name == name) { return &p; }
    }
    return nullptr;
}

std::span< billet::workload::component_spec const > profile_components(std::string_view name) {
    if ("random_read_4k" == name) { return billet::workload::random_read_4k_components(); }
    if ("postgresql" == name) { return billet::workload::profiles::postgresql_components(); }
    if ("pg_wal_commit" == name) { return billet::workload::profiles::pg_wal_commit_components(); }
    return {};
}

void log_multiline(std::string const& s) {
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) { LOGINFO("{}", line); }
    }
}

// Approval is granted by either:
//   - --allow-destructive on the command line, or
//   - an interactive 'yes' on a TTY.
// Refused otherwise (no flag + no TTY = no path to approval).
//
// The interactive prompt itself is written/read through stderr + stdin
// directly: log macros are async-safe but timing-fragile for prompt/getline.
bool destructive_approved(std::string_view profile, std::string_view device_path) {
    if (SISL_OPTIONS["allow-destructive"].as< bool >()) { return true; }
    if (0 == ::isatty(STDIN_FILENO)) {
        LOGERROR("destructive profile '{}' requires either --allow-destructive or an interactive TTY", profile);
        return false;
    }
    LOGWARN("profile '{}' will issue Write/Discard/Fsync ops to {}", profile, device_path);
    LOGWARN("Any data on this device (including filesystems) may be destroyed.");
    std::cerr << "Type 'yes' to continue: " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    return "yes" == answer;
}

} // namespace

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv, ENABLED_OPTIONS);

    // SetLogger consults SISL_OPTIONS["version"] internally and exits if set.
    // Calling it early lets every code path emit through sisl::logging
    // consistently; the sole cost is a logs/ directory on every invocation.
    sisl::logging::SetLogger(std::string(argv[0]),
                             BOOST_PP_STRINGIZE(PACKAGE_NAME), BOOST_PP_STRINGIZE(PACKAGE_VERSION));
    // Compact pattern: HH:MM:SS.ms + message. No level, source, or logger
    // name; this is a CLI tool, not a long-running service.
    spdlog::set_pattern("[%T.%e] %v");

    if (0 < SISL_OPTIONS.count("probe")) {
        auto const result = billet::engine::probe(SISL_OPTIONS["probe"].as< std::string >());
        if (!result) {
            LOGERROR("probe failed: {}", result.error().message());
            return 1;
        }
        log_multiline(billet::engine::to_string(*result));
        return 0;
    }

    if (0 < SISL_OPTIONS.count("device")) {
        auto const dev = billet::engine::probe(SISL_OPTIONS["device"].as< std::string >());
        if (!dev) {
            LOGERROR("probe failed: {}", dev.error().message());
            return 1;
        }

        auto const profile_name = SISL_OPTIONS["profile"].as< std::string >();
        auto const* const spec = lookup_profile(profile_name);
        if (nullptr == spec) {
            LOGERROR("unknown profile '{}'", profile_name);
            return 1;
        }

        if (spec->destructive && !destructive_approved(profile_name, dev->path.string())) {
            LOGERROR("aborted.");
            return 2;
        }

        uint32_t const worker_count = std::max< uint32_t >(1, SISL_OPTIONS["workers"].as< uint32_t >());
        if (1 != worker_count) {
            LOGERROR("billet currently runs single-worker only (got --workers={})", worker_count);
            return 1;
        }

        billet::engine::run_config cfg{};
        cfg.duration = std::chrono::seconds{SISL_OPTIONS["duration"].as< uint32_t >()};
        cfg.qd = SISL_OPTIONS["qd"].as< uint32_t >();
        cfg.pool_block_size = spec->pool_block_size;
        // Approval was checked above for destructive profiles. Mirroring
        // spec->destructive here keeps the engine's open mode at minimum
        // privilege: read-only profiles open O_RDONLY even after the gate
        // would have approved destructive use.
        cfg.destructive_allowed = spec->destructive;

        auto const started_at = billet::report::iso8601_now();

        auto const components = profile_components(profile_name);
        auto const run_id     = billet::report::make_ulid();

        std::function< exec::task< void >(billet::engine::workload_ctx&) > builder;
        if ("random_read_4k" == profile_name) {
            builder = [dev_size = dev->size_bytes,
                       cfg_qd   = cfg.qd](billet::engine::workload_ctx& ctx) -> exec::task< void > {
                co_await billet::workload::random_read_4k_run(ctx, dev_size, /*block_size=*/4096, cfg_qd,
                                                              /*rng_seed=*/0);
            };
        } else if ("postgresql" == profile_name) {
            billet::workload::profiles::postgresql_config pg{};
            pg.device_size_bytes  = dev->size_bytes;
            pg.readers            = SISL_OPTIONS["pg-readers"].as< uint32_t >();
            pg.reader_target_iops = SISL_OPTIONS["pg-reader-iops"].as< uint32_t >();
            pg.writers            = SISL_OPTIONS["pg-writers"].as< uint32_t >();
            pg.writer_target_iops = SISL_OPTIONS["pg-writer-iops"].as< uint32_t >();
            pg.wal_bytes_per_sec  = uint64_t(SISL_OPTIONS["pg-wal-mb-per-sec"].as< uint32_t >()) << 20;
            pg.wal_fsync_every_ms = SISL_OPTIONS["pg-wal-fsync-ms"].as< uint32_t >();
            pg.ckpt_period_ms     = SISL_OPTIONS["pg-ckpt-period-ms"].as< uint32_t >();
            pg.ckpt_burst_bytes   = uint64_t(SISL_OPTIONS["pg-ckpt-burst-mb"].as< uint32_t >()) << 20;
            pg.hot_set_frac       = SISL_OPTIONS["pg-hot-set-frac"].as< double >();
            pg.locality           = SISL_OPTIONS["pg-locality"].as< double >();
            builder               = [pg, cfg_qd = cfg.qd](billet::engine::workload_ctx& ctx) -> exec::task< void > {
                co_await billet::workload::profiles::postgresql_run(ctx, pg, cfg_qd);
            };
        } else if ("pg_wal_commit" == profile_name) {
            billet::workload::profiles::pg_wal_commit_config pgwc{};
            pgwc.device_size_bytes       = dev->size_bytes;
            pgwc.write_size_bytes        = SISL_OPTIONS["pgwc-write-size"].as< uint32_t >();
            pgwc.region_per_session_bytes = uint64_t(SISL_OPTIONS["pgwc-region-mb"].as< uint32_t >()) << 20;
            builder = [pgwc, cfg_qd = cfg.qd](billet::engine::workload_ctx& ctx) -> exec::task< void > {
                co_await billet::workload::profiles::pg_wal_commit_run(ctx, pgwc, cfg_qd);
            };
        } else {
            LOGERROR("unknown profile '{}'", profile_name);
            return 1;
        }

        billet::engine::live_stats live;

        // Optional Prometheus exposition. The metrics group registers
        // with the sisl::metrics farm at construction; the sampler
        // pumps the inflight gauge from live_stats every 500 ms; the
        // HTTP server stays up until --metrics-drain-s post-run so a
        // final Prom scrape catches terminal counters. Destruction
        // order at scope end is sampler -> server -> group: the
        // sampler joins its thread before the group it samples
        // disappears, the server stops accepting connections, the
        // group deregisters from the farm.
        std::unique_ptr< billet::stats::group >   metrics;
        std::unique_ptr< billet::stats::server >  metrics_http;
        std::unique_ptr< billet::stats::sampler > metrics_sampler;
        if (auto const port = SISL_OPTIONS["metrics-port"].as< uint16_t >(); 0 < port) {
            // Prefix entity with --device-label when set so Grafana legends
            // read e.g. "raid0-md.01HX..." instead of just the ULID. The
            // ULID stays in the suffix so each run is still distinct and
            // sortable. Without a label the entity is the ULID alone --
            // current single-run dashboards keep working untouched.
            auto const& dev_label   = SISL_OPTIONS["device-label"].as< std::string >();
            auto const  metrics_id  = dev_label.empty() ? run_id : (dev_label + "." + run_id);
            metrics                 = std::make_unique< billet::stats::group >(metrics_id, components);
            metrics_http            = std::make_unique< billet::stats::server >(port);
            metrics_sampler         = std::make_unique< billet::stats::sampler >(*metrics, live.inflight,
                                                                                  std::chrono::milliseconds{500});
            LOGINFO("metrics: /metrics on :{} (entity={})", port, metrics_id);
        }

        std::expected< billet::engine::run_summary, std::error_condition > sum;
        {
            billet::cli::progress_reporter prog(live, cfg.duration, cfg.qd);
            sum = billet::engine::run(*dev, cfg, components, &live, builder, metrics.get());
        }
        if (!sum) {
            LOGERROR("run failed: {}", sum.error().message());
            return 1;
        }
        LOGINFO("workload:    {}", profile_name);
        billet::engine::log_summary(*dev, cfg, *sum);

        if (0 < SISL_OPTIONS.count("output")) {
            auto const out_path = std::filesystem::path(SISL_OPTIONS["output"].as< std::string >());
            double const secs = std::chrono::duration< double >(sum->elapsed).count();

            billet::report::run_record rec;
            rec.schema_version = "billet.run/1";
            rec.run_id         = run_id;
            rec.started_at     = started_at;
            rec.duration_s = secs;
            rec.host = billet::report::gather_host_info();

            rec.device.path = dev->path.string();
            rec.device.size_bytes = dev->size_bytes;
            rec.device.logical_block = dev->logical_block_size;
            rec.device.physical_block = dev->physical_block_size;
            rec.device.max_io_kb = dev->max_io_kb;
            rec.device.rotational = dev->rotational;
            rec.device.discard_supported = dev->discard_supported;
            rec.device.fua_supported = dev->fua_supported;
            rec.device.write_zeroes_supported = dev->write_zeroes_supported;
            rec.device.label = SISL_OPTIONS["device-label"].as< std::string >();

            rec.profile.name = profile_name;
            rec.profile.version = "1";
            // Self-describing: every CLI parameter that shapes the workload
            // is captured so JSON files are comparable across runs without
            // needing the original invocation.
            if ("random_read_4k" == profile_name) {
                rec.profile.params["block_size"] = "4096";
                rec.profile.params["workers"] = std::to_string(worker_count);
            } else if ("pg_wal_commit" == profile_name) {
                rec.profile.params["write_size_bytes"] =
                    std::to_string(SISL_OPTIONS["pgwc-write-size"].as< uint32_t >());
                rec.profile.params["region_per_session_mb"] =
                    std::to_string(SISL_OPTIONS["pgwc-region-mb"].as< uint32_t >());
                rec.profile.params["sessions"] = std::to_string(cfg.qd);
            } else if ("postgresql" == profile_name) {
                rec.profile.params["readers"] = std::to_string(SISL_OPTIONS["pg-readers"].as< uint32_t >());
                rec.profile.params["reader_target_iops"] =
                    std::to_string(SISL_OPTIONS["pg-reader-iops"].as< uint32_t >());
                rec.profile.params["writers"] = std::to_string(SISL_OPTIONS["pg-writers"].as< uint32_t >());
                rec.profile.params["writer_target_iops"] =
                    std::to_string(SISL_OPTIONS["pg-writer-iops"].as< uint32_t >());
                rec.profile.params["wal_mb_per_sec"] =
                    std::to_string(SISL_OPTIONS["pg-wal-mb-per-sec"].as< uint32_t >());
                rec.profile.params["wal_fsync_ms"] = std::to_string(SISL_OPTIONS["pg-wal-fsync-ms"].as< uint32_t >());
                rec.profile.params["ckpt_period_ms"] =
                    std::to_string(SISL_OPTIONS["pg-ckpt-period-ms"].as< uint32_t >());
                rec.profile.params["ckpt_burst_mb"] = std::to_string(SISL_OPTIONS["pg-ckpt-burst-mb"].as< uint32_t >());
                rec.profile.params["workers"] = std::to_string(worker_count);
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.4f", SISL_OPTIONS["pg-hot-set-frac"].as< double >());
                    rec.profile.params["hot_set_frac"] = buf;
                    std::snprintf(buf, sizeof(buf), "%.4f", SISL_OPTIONS["pg-locality"].as< double >());
                    rec.profile.params["locality"] = buf;
                }
            }

            rec.engine.name = "io_uring";
            rec.engine.qd_per_worker = cfg.qd;
            rec.engine.workers = sum->workers;
            rec.engine.sqpoll = false;
            rec.engine.o_direct = true;

            rec.results.summary.ops_total = sum->ops_completed;
            rec.results.summary.bytes_total = sum->bytes_completed;
            rec.results.summary.iops_mean = (0.0 < secs) ? (static_cast< double >(sum->ops_completed) / secs) : 0.0;
            rec.results.summary.throughput_mibs =
                (0.0 < secs) ? (static_cast< double >(sum->bytes_completed) / 1048576.0 / secs) : 0.0;
            rec.results.summary.errors = sum->errors;
            rec.results.summary.component_drops = sum->component_drops;

            // Per-op-kind breakdown: emit only the kinds the workload actually
            // touched. Empty kinds (e.g. Discard for read-only profiles) are
            // skipped so the JSON's `by_op` keyset reflects reality.
            for (size_t k = 0; billet::workload::k_op_kind_count > k; ++k) {
                auto const& d = sum->by_kind[k];
                if (0 == d.ops) { continue; }
                auto const* const name = billet::workload::op_kind_name(static_cast< billet::workload::op_kind >(k));
                rec.results.by_op[std::string{name}] = billet::report::op_stats_from_hdr(d.hdr.get(), d.ops, d.bytes);
            }

            // Per-(component, op_kind) breakdown. Walks the spec and the cell
            // vector together; cell ordering matches make_cell_layout()'s
            // iteration order (component_id outer, kinds inner). Cells with
            // ops=0 are skipped so the keyset reflects reality.
            {
                size_t cell = 0;
                for (auto const& spec : components) {
                    for (auto const kind : spec.kinds) {
                        if (cell >= sum->by_component_cell.size()) { break; }
                        auto const& d = sum->by_component_cell[cell];
                        if (0 < d.ops) {
                            auto const key = std::string{spec.json_name} + "." +
                                             billet::workload::op_kind_name(kind);
                            rec.results.by_component[key] =
                                billet::report::op_stats_from_hdr(d.hdr.get(), d.ops, d.bytes);
                        }
                        ++cell;
                    }
                }
            }

            auto const wr = billet::report::write_json(rec, out_path);
            if (!wr) {
                LOGERROR("json write failed: {}", wr.error().message());
                return 1;
            }
            LOGINFO("wrote {}", out_path.string());
        }

        // Drain happens after JSON + log_summary so the user gets the
        // run artefacts immediately. The blocking sleep is purely the
        // window for one more Prom scrape against terminal counters.
        if (metrics_http) {
            auto const drain_s = SISL_OPTIONS["metrics-drain-s"].as< uint32_t >();
            if (0 < drain_s) {
                LOGINFO("metrics: draining {}s before shutdown", drain_s);
                metrics_http->drain(std::chrono::seconds{drain_s});
            }
        }
        return 0;
    }

    LOGERROR("no action specified; use --probe <path> or --device <path>");
    return 2;
}
