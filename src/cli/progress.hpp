#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include <engine/ring_worker.hpp>

namespace billet::cli {

// Renders a one-line indicators::ProgressBar driven by a sampler thread.
// Polls live_stats every ~250ms and updates the bar's postfix with interval
// IOPS / throughput, current inflight, cumulative rx/tx, and error count.
//
// On a non-TTY stdout (e.g. piped to a file) the bar is suppressed entirely so
// captured output stays clean.
class progress_reporter {
public:
    progress_reporter(billet::engine::live_stats const& stats, std::chrono::seconds duration, uint32_t qd_target);
    ~progress_reporter();

    progress_reporter(progress_reporter const&) = delete;
    progress_reporter& operator=(progress_reporter const&) = delete;

private:
    void run() noexcept;

    billet::engine::live_stats const& _stats;
    std::chrono::seconds _duration;
    uint32_t _qd_target;
    std::atomic< bool > _done{false};
    std::thread _thread;
    bool _enabled;
};

} // namespace billet::cli
