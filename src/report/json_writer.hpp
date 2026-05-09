#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

#include <report/run_record.hpp>

struct hdr_histogram;

namespace billet::report {

// Serialize the record to JSON and write it to `path` atomically (tmpfile + rename).
std::expected< void, std::error_condition > write_json(run_record const& rec, std::filesystem::path const& path);

// Serialize to a JSON string (2-space indent). Useful for tests and stdout dumping.
std::string to_json_string(run_record const& rec);

// Build an op_stats from an HDR histogram, deriving percentiles in microseconds and
// the gzip+base64-encoded HDR payload (hdrhistogram-c log v1 format). `count` and
// `bytes` come from the engine summary because hdr_histogram does not track them.
op_stats op_stats_from_hdr(hdr_histogram* hist, uint64_t count, uint64_t bytes);

// 26-char Crockford base32 ULID: 48-bit ms timestamp + 80-bit randomness.
std::string make_ulid();

// "2026-05-09T15:50:00Z" UTC ISO 8601.
std::string iso8601_now();

// uname + /proc/cpuinfo + sysconf snapshot.
host_info gather_host_info();

} // namespace billet::report
