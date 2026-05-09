#include <report/json_writer.hpp>

#include <sys/utsname.h>
#include <unistd.h>

#include <hdr/hdr_histogram.h>
#include <hdr/hdr_histogram_log.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>

namespace billet::report {

namespace {

using json = nlohmann::json;

constexpr char k_crockford32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

json serialize_op(op_stats const& s) {
    return json{
        {"count", s.count},       {"bytes", s.bytes},         {"p50_us", s.p50_us}, {"p99_us", s.p99_us},
        {"p99_9_us", s.p99_9_us}, {"p99_99_us", s.p99_99_us}, {"max_us", s.max_us}, {"hdr_b64", s.hdr_b64},
    };
}

} // namespace

std::string make_ulid() {
    using ms_t = std::chrono::milliseconds;
    int64_t ts_ms = std::chrono::duration_cast< ms_t >(std::chrono::system_clock::now().time_since_epoch()).count();

    char out[27];
    out[26] = '\0';

    for (int i = 9; 0 <= i; --i) {
        out[i] = k_crockford32[ts_ms & 0x1F];
        ts_ms >>= 5;
    }

    std::random_device rd;
    for (int i = 25; 10 <= i; --i) {
        out[i] = k_crockford32[rd() & 0x1F];
    }

    return std::string(out, 26);
}

std::string iso8601_now() {
    auto const now = std::chrono::system_clock::now();
    auto const t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

host_info gather_host_info() {
    host_info h{};

    char hn[256] = {0};
    if (0 == ::gethostname(hn, sizeof(hn) - 1)) { h.hostname = hn; }

    struct utsname u{};
    if (0 == ::uname(&u)) { h.kernel = u.release; }

    long const cores = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (0 < cores) { h.cores = static_cast< uint32_t >(cores); }

    long const pages = ::sysconf(_SC_PHYS_PAGES);
    long const psz = ::sysconf(_SC_PAGE_SIZE);
    if (0 < pages && 0 < psz) {
        h.ram_gb = (static_cast< double >(pages) * static_cast< double >(psz)) / (1024.0 * 1024.0 * 1024.0);
    }

    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("model name")) {
            auto const colon = line.find(':');
            if (std::string::npos != colon) {
                auto v = line.substr(colon + 1);
                auto const s = v.find_first_not_of(" \t");
                if (std::string::npos != s) { v.erase(0, s); }
                h.cpu_model = v;
            }
            break;
        }
    }
    return h;
}

op_stats op_stats_from_hdr(hdr_histogram* hist, uint64_t count, uint64_t bytes) {
    op_stats s{};
    s.count = count;
    s.bytes = bytes;
    if (nullptr == hist) { return s; }

    s.p50_us = ::hdr_value_at_percentile(hist, 50.0) / 1000;
    s.p99_us = ::hdr_value_at_percentile(hist, 99.0) / 1000;
    s.p99_9_us = ::hdr_value_at_percentile(hist, 99.9) / 1000;
    s.p99_99_us = ::hdr_value_at_percentile(hist, 99.99) / 1000;
    s.max_us = ::hdr_max(hist) / 1000;

    char* encoded = nullptr;
    if (0 == ::hdr_log_encode(hist, &encoded) && nullptr != encoded) {
        s.hdr_b64 = encoded;
        ::free(encoded);
    }
    return s;
}

std::string to_json_string(run_record const& rec) {
    json j;
    j["schema_version"] = rec.schema_version;
    j["run_id"] = rec.run_id;
    j["started_at"] = rec.started_at;
    j["duration_s"] = rec.duration_s;

    j["host"] = json{
        {"hostname", rec.host.hostname}, {"kernel", rec.host.kernel}, {"cpu_model", rec.host.cpu_model},
        {"cores", rec.host.cores},       {"ram_gb", rec.host.ram_gb},
    };

    j["device"] = json{
        {"path", rec.device.path},
        {"size_bytes", rec.device.size_bytes},
        {"logical_block", rec.device.logical_block},
        {"physical_block", rec.device.physical_block},
        {"max_io_kb", rec.device.max_io_kb},
        {"rotational", rec.device.rotational},
        {"discard_supported", rec.device.discard_supported},
        {"fua_supported", rec.device.fua_supported},
        {"write_zeroes_supported", rec.device.write_zeroes_supported},
        {"label", rec.device.label},
    };

    j["profile"] = json{
        {"name", rec.profile.name},
        {"version", rec.profile.version},
        {"params", rec.profile.params},
    };

    j["engine"] = json{
        {"name", rec.engine.name},     {"qd_per_worker", rec.engine.qd_per_worker}, {"workers", rec.engine.workers},
        {"sqpoll", rec.engine.sqpoll}, {"o_direct", rec.engine.o_direct},
    };

    json by_op = json::object();
    for (auto const& [k, v] : rec.results.by_op) {
        by_op[k] = serialize_op(v);
    }
    json by_phase = json::object();
    for (auto const& [k, v] : rec.results.by_phase) {
        by_phase[k] = serialize_op(v);
    }
    json by_component = json::object();
    for (auto const& [k, v] : rec.results.by_component) {
        by_component[k] = serialize_op(v);
    }

    j["results"] = json{
        {"summary",
         json{
             {"ops_total", rec.results.summary.ops_total},
             {"bytes_total", rec.results.summary.bytes_total},
             {"iops_mean", rec.results.summary.iops_mean},
             {"throughput_mibs", rec.results.summary.throughput_mibs},
             {"errors", rec.results.summary.errors},
             {"component_drops", rec.results.summary.component_drops},
         }},
        {"by_op", by_op},
        {"by_phase", by_phase},
        {"by_component", by_component},
    };

    return j.dump(2);
}

std::expected< void, std::error_condition > write_json(run_record const& rec, std::filesystem::path const& path) {
    auto const body = to_json_string(rec);

    auto tmp = path;
    tmp += ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { return std::unexpected(std::make_error_condition(std::errc::permission_denied)); }
        f.write(body.data(), static_cast< std::streamsize >(body.size()));
        if (!f) { return std::unexpected(std::make_error_condition(std::errc::io_error)); }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { return std::unexpected(ec.default_error_condition()); }
    return {};
}

} // namespace billet::report
