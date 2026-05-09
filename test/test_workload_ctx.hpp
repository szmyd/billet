#pragma once

// In-process test harness that satisfies the duck-typed Ctx contract used
// by the templated profile bodies (workload/*_impl.hpp). Backs onto
// sisl::async::manual_scheduler so timer-driven workloads (Poisson
// emitters, periodic checkpointers, WAL fsync intervals) advance through
// virtual time only when the test calls step().
//
// Two completion modes:
//   - Synchronous (default): submit_op records the op into the log and
//     completes immediately at virtual now_ns(). Suits "did the right
//     ops get emitted with the right intended_ts" assertions.
//   - Parked: when hold_when(predicate) is configured and the predicate
//     returns true for an op, submit_op records the issue but defers
//     completion until release_all_pending() fires. Suits "did the
//     emitter wait for prior work before issuing the next op" -- e.g.
//     pg_wal's writes_scope.when_empty drain before fsync.
//
// Reentrancy note (parked mode): release_all_pending() fires set_value
// on each parked receiver, which resumes coroutines inline. Those
// coroutines may issue further submit_op calls that push onto _pending
// before the release pass returns. release_all_pending() handles this
// by swapping _pending out into a local deque each round and draining
// the local; if _pending re-grew during the drain, it loops. A
// pathological loop is bounded by the watchdog below.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include <sisl/async/manual_scheduler.hpp>

#include <billet/workload/workload.hpp>

namespace billet::test {

struct test_completion {
    workload::op_kind kind{workload::op_kind::read};
    uint16_t          component_id{0};
    uint64_t          bytes{0};
    int64_t           latency_ns{0};
    uint64_t          intended_ts_ns{0};
    bool              ok{true};
};

struct emitted_op {
    workload::op_kind kind{};
    uint16_t          component_id{0};
    uint64_t          offset{0};
    uint32_t          len{0};
    uint64_t          intended_ts_ns{0};
    uint64_t          issued_ts_ns{0}; // virtual time at submit_op start
};

class test_workload_ctx {
public:
    test_workload_ctx() noexcept = default;

    test_workload_ctx(test_workload_ctx const&)            = delete;
    test_workload_ctx& operator=(test_workload_ctx const&) = delete;

    // ---- Ctx contract surface used by templated profile bodies --------

    uint64_t now_ns() const noexcept { return _sched.now_ns(); }

    // True when either the test has explicitly requested stop or virtual
    // time has crossed the configured deadline. The deadline lets tests
    // drive a profile to a natural exit by stepping past it; explicit
    // stop() is the escape hatch when the test wants to terminate early
    // without waiting for the deadline timer.
    bool stopped() const noexcept { return _stopped || _sched.now_ns() >= _deadline_ns; }

    sisl::async::manual_scheduler& scheduler() noexcept { return _sched; }

    // Step `total` virtual time as repeated chunks of `chunk` each. sisl's
    // manual_scheduler::step advances _now to the post-step value before
    // draining timers, so every timer in the interval observes the same
    // (post-step) virtual time. Profiles that recheck state between
    // emissions -- e.g. pg_wal_emitter's `time_due` gate, which uses
    // ctx.now_ns() inside the fire path -- collapse multiple cycles into
    // one when stepped in a single jump. Chunked stepping advances _now
    // monotonically across the interval so the gate fires at the right
    // boundary.
    void step_chunked(std::chrono::nanoseconds total, std::chrono::nanoseconds chunk) {
        auto remaining = total;
        while (remaining.count() > 0) {
            auto const amt = std::min(remaining, chunk);
            _sched.step(amt);
            remaining -= amt;
        }
    }

    // ---- submit_op sender ---------------------------------------------

    using release_fn = void (*)(void*) noexcept;

    template < typename Receiver >
    struct submit_op_state {
        test_workload_ctx* _ctx;
        workload::op       _work;
        Receiver           _receiver;

        void start() noexcept {
            uint64_t const now = _ctx->_sched.now_ns();
            // Issue is the visible event -- record it now whether we
            // park or fire. Tests asserting "fsync issued before/after
            // X" rely on the log capturing this ordering even when
            // completion is deferred.
            _ctx->_log.push_back(emitted_op{
                _work.kind, _work.component_id, _work.offset, _work.len, _work.intended_ts_ns, now,
            });

            if (_ctx->_hold_predicate && _ctx->_hold_predicate(_work)) {
                _ctx->_pending.push_back({&submit_op_state::release_thunk, this});
                return; // parked: completion fires from release_all_pending
            }

            stdexec::set_value(std::move(_receiver), make_completion(now));
        }

        // Parked-mode completion. Builds the completion at the virtual
        // time of release, so latency reflects the (deferred) drain
        // window the test simulated. The op_state's storage lives
        // inside the awaiting coroutine's frame; that frame is alive
        // because the coroutine is suspended on this very co_await,
        // and stdexec::set_value is what resumes it. Once set_value
        // returns, control here is irrelevant -- the thunk does not
        // touch the op_state again.
        static void release_thunk(void* p) noexcept {
            auto* const self = static_cast< submit_op_state* >(p);
            uint64_t const now = self->_ctx->_sched.now_ns();
            stdexec::set_value(std::move(self->_receiver), self->make_completion(now));
        }

        test_completion make_completion(uint64_t now) const noexcept {
            test_completion out{};
            out.kind           = _work.kind;
            out.component_id   = _work.component_id;
            out.intended_ts_ns = _work.intended_ts_ns;
            out.bytes          = (workload::op_kind::fsync == _work.kind) ? 0 : _work.len;
            // latency = virtual now - intended, mirroring the
            // production engine's "completion - intended" rule. With
            // synchronous completion this is the queueing delay; with
            // parked completion it captures the drain interval the
            // test imposed.
            out.latency_ns =
                static_cast< int64_t >(now) - static_cast< int64_t >(_work.intended_ts_ns);
            out.ok = true;
            return out;
        }
    };

    struct submit_op_sender {
        using sender_concept        = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures< stdexec::set_value_t(test_completion) >;

        test_workload_ctx* _ctx;
        workload::op       _work;

        template < typename Receiver >
        submit_op_state< std::decay_t< Receiver > > connect(Receiver&& r) const noexcept {
            return {_ctx, _work, std::forward< Receiver >(r)};
        }
    };

    [[nodiscard]] submit_op_sender submit_op(workload::op const& o) noexcept { return {this, o}; }

    // ---- Test driver knobs --------------------------------------------

    // Set the virtual deadline at which stopped() flips true. Default is
    // never; explicit stop() is the alternative.
    void set_deadline_ns(uint64_t ns) noexcept { _deadline_ns = ns; }

    // Force stopped() true on the next observation. Used to terminate an
    // open-ended emitter without waiting for the deadline timer.
    void stop() noexcept { _stopped = true; }

    // Install or clear a predicate that diverts matching submit_op calls
    // into the pending queue. nullptr clears (back to synchronous mode
    // for all ops). Useful for asserting drain ordering: hold the ops
    // a profile is supposed to wait on, drive virtual time past the
    // would-be completion deadline, assert no downstream op fired,
    // then release.
    void hold_when(std::function< bool(workload::op const&) > pred) noexcept {
        _hold_predicate = std::move(pred);
    }

    // Drain every parked completion, in issue order. set_value resumes
    // coroutines inline; if those resumptions issue more submit_op calls
    // that match the hold predicate (and therefore re-park), the loop
    // iterates until _pending is empty. The watchdog is a sanity bound
    // on pathological cycles (re-park on every release).
    void release_all_pending() {
        constexpr size_t k_watchdog_rounds = 64;
        for (size_t round = 0; k_watchdog_rounds > round; ++round) {
            if (_pending.empty()) { return; }
            std::deque< pending_entry > batch;
            batch.swap(_pending);
            for (auto& e : batch) { e.fn(e.ctx); }
        }
        // If we hit the watchdog the test is misconfigured -- ops keep
        // re-parking. Leave _pending alone so the test's own assertions
        // surface the misconfiguration rather than masking it here.
    }

    size_t pending_count() const noexcept { return _pending.size(); }

    std::vector< emitted_op > const& log() const noexcept { return _log; }
    void                            clear_log() noexcept { _log.clear(); }

private:
    struct pending_entry {
        release_fn fn;
        void*      ctx;
    };

    sisl::async::manual_scheduler                  _sched;
    uint64_t                                       _deadline_ns{std::numeric_limits< uint64_t >::max()};
    bool                                           _stopped{false};
    std::vector< emitted_op >                      _log;
    std::function< bool(workload::op const&) >     _hold_predicate{};
    std::deque< pending_entry >                    _pending;
};

} // namespace billet::test
