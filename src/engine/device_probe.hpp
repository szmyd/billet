#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

namespace billet::engine {

struct device_info {
    std::filesystem::path path;
    uint64_t size_bytes{0};
    uint32_t logical_block_size{0};     // BLKSSZGET
    uint32_t physical_block_size{0};    // BLKPBSZGET (may exceed 4K on virtual devices)
    uint32_t max_io_kb{0};              // queue/max_hw_sectors_kb
    bool rotational{false};             // BLKROTATIONAL
    bool discard_supported{false};      // queue/discard_granularity > 0
    bool fua_supported{false};          // queue/fua
    bool write_zeroes_supported{false}; // queue/write_zeroes_max_bytes > 0
};

// Open `dev_path` read-only, validate it is a block device, and probe its geometry
// and capabilities via BLK* ioctls and sysfs queue attributes. Refuses non-block
// inputs (/dev/null, /dev/zero, regular files) with std::errc::not_supported.
std::expected< device_info, std::error_condition > probe(std::filesystem::path const& dev_path);

// Human-readable rendering, one `key: value` per line, trailing newline.
std::string to_string(device_info const& info);

} // namespace billet::engine
