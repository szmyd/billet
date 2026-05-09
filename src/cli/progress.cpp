#include <cli/progress.hpp>

#include <unistd.h>

#include <algorithm>
#include <iomanip>
#include <sstream>

#include <indicators/progress_bar.hpp>

namespace billet::cli {

namespace {

using clock_type = std::chrono::steady_clock;

constexpr auto k_sample_interval = std::chrono::milliseconds{250};
constexpr int k_bar_width = 24;

// Compact rate formatting -- "219.1k", "1.20M", "47".
std::string fmt_rate(double v) {
    std::ostringstream os;
    os << std::fixed;
    if (1e6 <= v) {
        os << std::setprecision(2) << (v / 1e6) << "M";
    } else if (1e3 <= v) {
        os << std::setprecision(1) << (v / 1e3) << "k";
    } else {
        os << std::setprecision(0) << v;
    }
    return os.str();
}

// Bytes-per-second to MiB/s or GiB/s.
std::string fmt_bps(double bps) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1);
    double const mibps = bps / 1048576.0;
    if (1024.0 <= mibps) {
        os << (mibps / 1024.0) << " GiB/s";
    } else {
        os << mibps << " MiB/s";
    }
    return os.str();
}

// Cumulative bytes to a short binary-prefixed string.
std::string fmt_bytes(uint64_t b) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1);
    double const v = static_cast< double >(b);
    if (v >= (1024.0 * 1024.0 * 1024.0)) {
        os << (v / (1024.0 * 1024.0 * 1024.0)) << "G";
    } else if (v >= (1024.0 * 1024.0)) {
        os << (v / (1024.0 * 1024.0)) << "M";
    } else if (v >= 1024.0) {
        os << (v / 1024.0) << "K";
    } else {
        os << std::setprecision(0) << v << "B";
    }
    return os.str();
}

} // namespace

progress_reporter::progress_reporter(billet::engine::live_stats const& stats, std::chrono::seconds duration,
                                     uint32_t qd_target) :
        _stats(stats), _duration(duration), _qd_target(qd_target), _enabled(0 != ::isatty(STDOUT_FILENO)) {
    if (_enabled) {
        _thread = std::thread([this] { run(); });
    }
}

progress_reporter::~progress_reporter() {
    _done.store(true, std::memory_order_release);
    if (_thread.joinable()) { _thread.join(); }
}

void progress_reporter::run() noexcept {
    indicators::ProgressBar bar{
        indicators::option::BarWidth{k_bar_width},
        indicators::option::Start{"["},
        indicators::option::Fill{"="},
        indicators::option::Lead{">"},
        indicators::option::Remainder{" "},
        indicators::option::End{"]"},
        indicators::option::ShowPercentage{true},
        indicators::option::ShowElapsedTime{false},
        indicators::option::ShowRemainingTime{false},
        indicators::option::PostfixText{""},
    };

    auto const start = clock_type::now();
    auto const total_s = std::max(0.001, std::chrono::duration< double >(_duration).count());

    uint64_t last_ops = 0;
    uint64_t last_rx = 0;
    uint64_t last_tx = 0;
    auto last_t = start;

    // Exponential moving average of inflight. Open-loop workloads with
    // device latency below the per-op rate spend most samples at inflight=0
    // (the schedule produces ops slower than the device serves them);
    // showing instantaneous alone is misleading. alpha=0.2 with a 250ms
    // sample interval gives a half-life around 0.78s -- responsive to
    // bursts, smooths out single-sample noise.
    constexpr double k_inflight_ema_alpha = 0.2;
    double inflight_ema = 0.0;
    bool ema_seeded = false;

    while (!_done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(k_sample_interval);

        auto const now = clock_type::now();
        double const elapsed_s = std::chrono::duration< double >(now - start).count();
        double const pct = std::min(1.0, elapsed_s / total_s);

        uint64_t const ops = _stats.ops.load(std::memory_order_relaxed);
        uint64_t const rx = _stats.rx_bytes.load(std::memory_order_relaxed);
        uint64_t const tx = _stats.tx_bytes.load(std::memory_order_relaxed);
        uint64_t const errors = _stats.errors.load(std::memory_order_relaxed);
        uint32_t const inflight = _stats.inflight.load(std::memory_order_relaxed);

        double const dt = std::chrono::duration< double >(now - last_t).count();
        double const iops = (0.0 < dt) ? (static_cast< double >(ops - last_ops) / dt) : 0.0;
        double const rxbps = (0.0 < dt) ? (static_cast< double >(rx - last_rx) / dt) : 0.0;
        double const txbps = (0.0 < dt) ? (static_cast< double >(tx - last_tx) / dt) : 0.0;

        last_ops = ops;
        last_rx = rx;
        last_tx = tx;
        last_t = now;

        if (!ema_seeded) {
            inflight_ema = static_cast< double >(inflight);
            ema_seeded = true;
        } else {
            inflight_ema =
                k_inflight_ema_alpha * static_cast< double >(inflight) + (1.0 - k_inflight_ema_alpha) * inflight_ema;
        }

        std::ostringstream pf;
        pf << std::fixed << std::setprecision(1) << elapsed_s << "/" << total_s << "s | " << fmt_rate(iops)
           << " iops | " << fmt_bps(rxbps + txbps) << " | "
           << "qd=" << inflight << "(ema " << std::fixed << std::setprecision(1) << inflight_ema << ")/" << _qd_target
           << " | "
           << "rx=" << fmt_bytes(rx);
        if (0 < tx) { pf << " tx=" << fmt_bytes(tx); }
        pf << " | err=" << errors;

        bar.set_option(indicators::option::PostfixText{pf.str()});
        // Cap mid-run progress at 99: indicators emits a finalization newline
        // every time progress reaches its max, so calling set_progress(100)
        // more than once (e.g. while the worker drains inflight ops past the
        // deadline) prints duplicate completed lines. The single 100% line is
        // emitted by mark_as_completed below.
        bar.set_progress(std::min< size_t >(99, static_cast< size_t >(pct * 100.0)));
    }

    bar.mark_as_completed();
}

} // namespace billet::cli
