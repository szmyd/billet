#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include <liburing.h>

#include <hdr/hdr_histogram.h>

#include <stdexec/execution.hpp>

#include <sisl/async/cqe_state.hpp>
#include <sisl/async/io_uring_scheduler.hpp>

#include <billet/workload/workload.hpp>
#include <engine/aligned_buf.hpp>
#include <engine/hdr.hpp>
#include <engine/ring_worker.hpp>
#include <stats/stats.hpp>

namespace billet::engine {

// Result of a single op submission.
struct sender_completion {
    billet::workload::op_kind kind{billet::workload::op_kind::read};
    uint16_t component_id{0};
    uint64_t bytes{0};
    int64_t latency_ns{0};
    uint64_t intended_ts_ns{0};
    bool ok{true}; // false on -errno from io_uring or refused op kinds
};

class workload_ctx {
public:
    // Per-worker accumulator. by_kind feeds run_summary.by_kind; by_cell
    // feeds run_summary.by_component_cell. Cells are indexed by the
    // cell_layout derived from the profile's component_spec.
    struct accum {
        std::array< uint64_t, billet::workload::k_op_kind_count > ops_by_kind{};
        std::array< uint64_t, billet::workload::k_op_kind_count > bytes_by_kind{};
        std::array< hdr_ptr, billet::workload::k_op_kind_count > hdrs{};
        std::vector< uint64_t > cell_ops{};
        std::vector< uint64_t > cell_bytes{};
        std::vector< hdr_ptr > cell_hdrs{};
        uint64_t errors{0};
        uint64_t component_drops{0};
    };

    workload_ctx(sisl::async::io_uring_scheduler& sched, aligned_buf_pool& pool, int fd, uint64_t deadline_ns,
                 accum& a, billet::workload::cell_layout const& layout, uint32_t qd_max,
                 live_stats* live = nullptr, billet::stats::group* metrics = nullptr) noexcept :
            _sched(&sched), _pool(&pool), _fd(fd), _deadline_ns(deadline_ns), _accum(&a), _layout(&layout),
            _live(live), _metrics(metrics), _qd_max(qd_max) {}

    workload_ctx(workload_ctx const&)            = delete;
    workload_ctx& operator=(workload_ctx const&) = delete;

    uint64_t now_ns() const noexcept {
        return static_cast< uint64_t >(
            std::chrono::duration_cast< std::chrono::nanoseconds >(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    bool stopped() const noexcept { return now_ns() >= _deadline_ns; }

    sisl::async::io_uring_scheduler& scheduler() noexcept { return *_sched; }

    // ----------- qd backpressure (sender-internal) -----------------------

    using waiter_fn = void (*)(void*) noexcept;
    struct waiter {
        waiter_fn fn;
        void*     ctx;
    };

    // Called by submit_op_state at start. Either acquires a qd slot
    // immediately (returns true) or parks the resume thunk on the
    // waiter queue (returns false). Single-threaded; safe to mutate
    // _qd_used and _waiters without atomics.
    bool try_acquire_qd_slot(waiter_fn resume_fn, void* op_state_ctx) noexcept {
        if (_qd_used < _qd_max) {
            ++_qd_used;
            return true;
        }
        _waiters.push_back({resume_fn, op_state_ctx});
        return false;
    }

    // Called from the cqe_state on_complete thunk. Releases a slot and,
    // if any op_state was parked, hands the slot directly to the head of
    // the queue and resumes its do_submit (no decrement -- the slot is
    // transferred). Otherwise decrements _qd_used.
    void release_qd_slot() noexcept {
        if (!_waiters.empty()) {
            waiter const w = _waiters.front();
            _waiters.pop_front();
            w.fn(w.ctx);
        } else {
            --_qd_used;
        }
    }

    // ------------- submit_op sender ------------------------------------

    template < typename Receiver >
    struct submit_op_state {
        workload_ctx*          _ctx;
        billet::workload::op   _work;
        Receiver               _receiver;
        sisl::async::cqe_state _state{};
        aligned_buf            _buf{};
        sender_completion      _result{};

        void start() noexcept {
            _result.kind           = _work.kind;
            _result.component_id   = _work.component_id;
            _result.intended_ts_ns = _work.intended_ts_ns;

            // discard / write_zeroes are not yet wired through io_uring.
            // Refuse explicitly rather than silently NOP'ing -- a NOP
            // would count as a successful op of the requested kind and
            // taint downstream stats. No qd slot consumed.
            using enum billet::workload::op_kind;
            if (discard == _work.kind || write_zeroes == _work.kind) {
                ++_ctx->_accum->errors;
                if (nullptr != _ctx->_live) { _ctx->_live->errors.fetch_add(1, std::memory_order_relaxed); }
                if (nullptr != _ctx->_metrics) { _ctx->_metrics->on_op_error(); }
                _result.ok         = false;
                _result.bytes      = 0;
                _result.latency_ns = 0;
                stdexec::set_value(std::move(_receiver), _result);
                return;
            }

            // qd backpressure: take a slot immediately or park until a
            // completion frees one. Without this gate, a profile that
            // spawns many submit_op tasks (e.g. WAL between fsyncs) can
            // overrun the io_uring SQ ring and deref a null SQE.
            if (_ctx->try_acquire_qd_slot(&submit_op_state::on_slot_granted, this)) {
                do_submit();
            }
        }

        // Static thunk invoked by workload_ctx::release_qd_slot when this
        // op_state was the head of the wait queue.
        static void on_slot_granted(void* p) noexcept {
            auto* const self = static_cast< submit_op_state* >(p);
            self->do_submit();
        }

        void do_submit() noexcept {
            using enum billet::workload::op_kind;
            if (fsync != _work.kind) { _buf = _ctx->_pool->acquire(_work.len); }
            if (nullptr != _ctx->_live) { _ctx->_live->inflight.fetch_add(1, std::memory_order_relaxed); }

            _state._on_complete_ctx = this;
            _state._on_complete     = +[](void* p, int res) noexcept {
                auto* const                     self        = static_cast< submit_op_state* >(p);
                workload_ctx&                   ctx         = *self->_ctx;
                accum&                          accumulator = *ctx._accum;
                billet::workload::op_kind const kind        = self->_work.kind;
                uint16_t const                  cid         = self->_work.component_id;
                uint64_t const                  completion_ns = ctx.now_ns();

                self->_result.latency_ns =
                    static_cast< int64_t >(completion_ns) - static_cast< int64_t >(self->_work.intended_ts_ns);

                if (nullptr != ctx._live) { ctx._live->inflight.fetch_sub(1, std::memory_order_relaxed); }

                if (0 > res) {
                    ++accumulator.errors;
                    if (nullptr != ctx._live) { ctx._live->errors.fetch_add(1, std::memory_order_relaxed); }
                    if (nullptr != ctx._metrics) { ctx._metrics->on_op_error(); }
                    self->_result.ok = false;
                } else {
                    self->_result.bytes = static_cast< uint64_t >(res);
                    self->_result.ok    = true;
                    size_t const k      = billet::workload::op_kind_index(kind);
                    ++accumulator.ops_by_kind[k];
                    accumulator.bytes_by_kind[k] += self->_result.bytes;
                    if (accumulator.hdrs[k]) {
                        ::hdr_record_value(accumulator.hdrs[k].get(), self->_result.latency_ns);
                    }

                    bool const    cell_in_range = cid < ctx._layout->idx.size();
                    int32_t const cell          = cell_in_range ? ctx._layout->idx[cid][k] : -1;
                    if (0 <= cell) {
                        auto const u = static_cast< size_t >(cell);
                        ++accumulator.cell_ops[u];
                        accumulator.cell_bytes[u] += self->_result.bytes;
                        if (accumulator.cell_hdrs[u]) {
                            ::hdr_record_value(accumulator.cell_hdrs[u].get(), self->_result.latency_ns);
                        }
                    } else if (!ctx._layout->idx.empty()) {
                        ++accumulator.component_drops;
                    }

                    if (nullptr != ctx._live) {
                        ctx._live->ops.fetch_add(1, std::memory_order_relaxed);
                        if (billet::workload::op_kind::write == kind) {
                            ctx._live->tx_bytes.fetch_add(self->_result.bytes, std::memory_order_relaxed);
                        } else if (billet::workload::op_kind::read == kind) {
                            ctx._live->rx_bytes.fetch_add(self->_result.bytes, std::memory_order_relaxed);
                        }
                    }

                    if (nullptr != ctx._metrics) {
                        ctx._metrics->on_op_complete(cid, kind, self->_result.latency_ns, self->_result.bytes);
                    }
                }
                // Free the qd slot before set_value: the receiver may
                // chain immediately into another submit_op which will
                // try to acquire a slot.
                ctx.release_qd_slot();
                sender_completion const out = self->_result;
                stdexec::set_value(std::move(self->_receiver), out);
            };

            ::io_uring_sqe* const sqe = ::io_uring_get_sqe(_ctx->_sched->ring());
            if (nullptr == sqe) {
                // Defensive: with correct qd accounting + ring sized
                // above qd this is unreachable, but ring backpressure
                // from timer / control SQEs could in principle hit it.
                // Surface as an error completion rather than UB.
                ++_ctx->_accum->errors;
                if (nullptr != _ctx->_live) {
                    _ctx->_live->errors.fetch_add(1, std::memory_order_relaxed);
                    _ctx->_live->inflight.fetch_sub(1, std::memory_order_relaxed);
                }
                if (nullptr != _ctx->_metrics) { _ctx->_metrics->on_op_error(); }
                _result.ok = false;
                _ctx->release_qd_slot();
                sender_completion const out = _result;
                stdexec::set_value(std::move(_receiver), out);
                return;
            }

            switch (_work.kind) {
            case read:
                ::io_uring_prep_read(sqe, _ctx->_fd, _buf.data(), _work.len, _work.offset);
                break;
            case write:
                ::io_uring_prep_write(sqe, _ctx->_fd, _buf.data(), _work.len, _work.offset);
                break;
            case fsync:
                ::io_uring_prep_fsync(sqe, _ctx->_fd, 0);
                break;
            default:
                ::io_uring_prep_nop(sqe);
                break;
            }
            ::io_uring_sqe_set_data64(sqe, sisl::async::encode_managed_user_data(&_state));
            // Submit deferred to scheduler.poll_once for batching.
        }
    };

    struct submit_op_sender {
        using sender_concept        = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures< stdexec::set_value_t(sender_completion) >;

        workload_ctx*        _ctx;
        billet::workload::op _work;

        template < typename Receiver >
        submit_op_state< std::decay_t< Receiver > > connect(Receiver&& r) const noexcept {
            return {_ctx, _work, std::forward< Receiver >(r), {}, {}, {}};
        }
    };

    [[nodiscard]] submit_op_sender submit_op(billet::workload::op const& o) noexcept {
        return {this, o};
    }

private:
    sisl::async::io_uring_scheduler*    _sched;
    aligned_buf_pool*                   _pool;
    int                                 _fd;
    uint64_t                            _deadline_ns;
    accum*                              _accum;
    billet::workload::cell_layout const* _layout;
    live_stats*                         _live;
    billet::stats::group*               _metrics;
    uint32_t                            _qd_max;
    uint32_t                            _qd_used{0};
    std::deque< waiter >                _waiters{};
};

} // namespace billet::engine
