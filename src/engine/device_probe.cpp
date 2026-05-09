#include <engine/device_probe.hpp>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <sstream>

namespace billet::engine {

namespace {

std::error_condition errc_from(int errnum) noexcept { return std::generic_category().default_error_condition(errnum); }

// Read one unsigned integer from a sysfs attribute. Missing/unreadable files
// degrade silently to `fallback` -- partitions and synthetic devices don't
// always expose every queue attribute.
uint64_t read_sysfs_u64(std::filesystem::path const& p, uint64_t fallback = 0) noexcept {
    std::ifstream f(p);
    uint64_t v{};
    return (f >> v) ? v : fallback;
}

std::filesystem::path sysfs_queue_dir(std::filesystem::path const& dev) {
    // /dev/sda -> /sys/class/block/sda/queue
    return std::filesystem::path("/sys/class/block") / dev.filename() / "queue";
}

} // namespace

std::expected< device_info, std::error_condition > probe(std::filesystem::path const& dev_path) {
    int const fd = ::open(dev_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (0 > fd) { return std::unexpected(errc_from(errno)); }

    struct stat st{};
    if (0 != ::fstat(fd, &st)) {
        int const err = errno;
        ::close(fd);
        return std::unexpected(errc_from(err));
    }
    if (!S_ISBLK(st.st_mode)) {
        ::close(fd);
        return std::unexpected(std::make_error_condition(std::errc::not_supported));
    }

    device_info info{};
    info.path = dev_path;

    if (0 != ::ioctl(fd, BLKGETSIZE64, &info.size_bytes)) {
        int const err = errno;
        ::close(fd);
        return std::unexpected(errc_from(err));
    }

    int logical{0};
    if (0 != ::ioctl(fd, BLKSSZGET, &logical)) {
        int const err = errno;
        ::close(fd);
        return std::unexpected(errc_from(err));
    }
    info.logical_block_size = static_cast< uint32_t >(logical);

    int physical{0};
    if (0 == ::ioctl(fd, BLKPBSZGET, &physical)) {
        info.physical_block_size = static_cast< uint32_t >(physical);
    } else {
        // BLKPBSZGET landed in 2.6.32; fall back to logical for stripped-down kernels.
        info.physical_block_size = info.logical_block_size;
    }

    unsigned long rotational{0};
    if (0 == ::ioctl(fd, BLKROTATIONAL, &rotational)) { info.rotational = (0 != rotational); }

    ::close(fd);

    auto const qd = sysfs_queue_dir(dev_path);
    info.max_io_kb = static_cast< uint32_t >(read_sysfs_u64(qd / "max_hw_sectors_kb"));
    info.discard_supported = (0 != read_sysfs_u64(qd / "discard_granularity"));
    info.fua_supported = (0 != read_sysfs_u64(qd / "fua"));
    info.write_zeroes_supported = (0 != read_sysfs_u64(qd / "write_zeroes_max_bytes"));

    return info;
}

std::string to_string(device_info const& info) {
    auto const yn = [](bool b) { return b ? "yes" : "no"; };
    std::ostringstream os;
    os << "path:                   " << info.path.string() << "\n"
       << "size_bytes:             " << info.size_bytes << "\n"
       << "logical_block_size:     " << info.logical_block_size << "\n"
       << "physical_block_size:    " << info.physical_block_size << "\n"
       << "max_io_kb:              " << info.max_io_kb << "\n"
       << "rotational:             " << yn(info.rotational) << "\n"
       << "discard_supported:      " << yn(info.discard_supported) << "\n"
       << "fua_supported:          " << yn(info.fua_supported) << "\n"
       << "write_zeroes_supported: " << yn(info.write_zeroes_supported) << "\n";
    return os.str();
}

} // namespace billet::engine
