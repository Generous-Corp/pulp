#include <pulp/format/graph_runtime_worker_pool.hpp>

#include <pulp/audio/workgroup.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

#include <cassert>
#include <chrono>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace pulp::format {
namespace {

constexpr auto kHotIdleWindow = std::chrono::microseconds(5000);
constexpr auto kColdIdleSleep = std::chrono::milliseconds(1);
constexpr std::uint32_t kIdleSpinBeforeBackoff = 256;

// Even static split of [0, count) across `workers` participants: participant w
// owns [w*count/workers, (w+1)*count/workers). Balanced to within one task.
struct Range {
    std::uint32_t begin;
    std::uint32_t end;
};
Range range_for(std::uint32_t worker_index, std::uint32_t workers,
                std::uint32_t count) noexcept {
    const std::uint64_t c = count;
    const std::uint32_t begin =
        static_cast<std::uint32_t>(c * worker_index / workers);
    const std::uint32_t end =
        static_cast<std::uint32_t>(c * (worker_index + 1) / workers);
    return {begin, end};
}

void cpu_relax() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm64__)
    __asm__ __volatile__("yield");
#endif
}

} // namespace

GraphRuntimeWorkerPool::~GraphRuntimeWorkerPool() { stop(); }

bool GraphRuntimeWorkerPool::start(std::uint32_t worker_count) {
    stop();
    if (worker_count == 0) return false;
    worker_count_ = worker_count;
    stopping_.store(false, std::memory_order_release);
    epoch_.store(0, std::memory_order_release);
    completed_.store(0, std::memory_order_release);
    cold_transition_gate_.store(false, std::memory_order_release);
    reheat_requested_.store(false, std::memory_order_release);
    active_worker_threads_.store(0, std::memory_order_release);
    worker_backoff_count_.store(0, std::memory_order_release);
    worker_idle_sleep_count_.store(0, std::memory_order_release);
    completed_base_ = 0;
    // worker_count includes the run() caller as participant 0; spawn the rest.
    if (worker_count_ <= 1) {
        running_.store(true, std::memory_order_release);
        return true;
    }
    try {
        threads_.reserve(worker_count_ - 1);
        for (std::uint32_t w = 1; w < worker_count_; ++w) {
            threads_.emplace_back([this, w] { worker_loop(w); });
        }
    } catch (...) {
        stop();
        return false;
    }
    running_.store(true, std::memory_order_release);
    return true;
}

void GraphRuntimeWorkerPool::stop() noexcept {
    if (!threads_.empty()) {
        // Worker loops poll the epoch while hot and sleep while cold. This is
        // off-RT; join latency is bounded by kColdIdleSleep.
        stopping_.store(true, std::memory_order_release);
        reheat_requested_.store(true, std::memory_order_release);
        epoch_.fetch_add(1, std::memory_order_release);
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }
    worker_count_ = 0;
    active_worker_threads_.store(0, std::memory_order_release);
    cold_transition_gate_.store(false, std::memory_order_release);
    reheat_requested_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

void GraphRuntimeWorkerPool::set_audio_workgroup(void* workgroup) noexcept {
    if (worker_count_ == 0 && threads_.empty()) {
        audio_workgroup_.store(workgroup, std::memory_order_release);
    }
}

void GraphRuntimeWorkerPool::run(std::uint32_t task_count, TaskFn fn,
                                 void* context) noexcept {
    if (task_count == 0 || fn == nullptr) return;
    // No worker threads: run everything inline on the caller.
    if (worker_count_ <= 1 || threads_.empty()) {
        pulp::signal::ScopedFlushDenormals flush_denormals;
        for (std::uint32_t i = 0; i < task_count; ++i) fn(context, i);
        return;
    }

    bool gate_expected = false;
    if (!cold_transition_gate_.compare_exchange_strong(
            gate_expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        reheat_requested_.store(true, std::memory_order_release);
        pulp::signal::ScopedFlushDenormals flush_denormals;
        for (std::uint32_t i = 0; i < task_count; ++i) fn(context, i);
        return;
    }
    if (active_worker_threads_.load(std::memory_order_acquire) != worker_count_ - 1) {
        reheat_requested_.store(true, std::memory_order_release);
        cold_transition_gate_.store(false, std::memory_order_release);
        pulp::signal::ScopedFlushDenormals flush_denormals;
        for (std::uint32_t i = 0; i < task_count; ++i) fn(context, i);
        return;
    }
    reheat_requested_.store(false, std::memory_order_release);

    // run() must not overlap another run() (the lock-free barrier's correctness
    // depends on serialized batches); enforce it loudly in debug builds.
    assert(!in_run_.exchange(true, std::memory_order_relaxed) &&
           "GraphRuntimeWorkerPool::run() is not re-entrant and must not run concurrently");

    // Publish the batch, then release it to the polling workers via the epoch.
    // completed_ counts PARTICIPANTS finished (not tasks): every participant —
    // including one handed an empty range — bumps it by 1 after it has read the
    // published batch (task_count_/fn_/context_) in run_range. Waiting for all
    // worker_count_ participants therefore guarantees no straggler is still
    // reading those members when the next batch overwrites them. (Counting tasks
    // instead lets an empty-range worker's +0 go un-awaited, so it could still be
    // reading task_count_ as the next run() writes it — a data race.) completed_
    // is monotonic across batches; this one is done at base + worker_count_.
    const std::uint64_t target = completed_base_ + worker_count_;
    fn_ = fn;
    context_ = context;
    task_count_ = task_count;
    epoch_.fetch_add(1, std::memory_order_release);

    // The caller is participant 0.
    run_range(0);

    // Spin until every participant has registered done (all task writes are then
    // visible via the completed_ acquire/release barrier).
    // `< target` (not `!=`): completed_ reaches target exactly, but a `<` guard
    // can never deadlock the spin if the count ever overshoots.
    std::uint32_t spins = 0;
    while (completed_.load(std::memory_order_acquire) < target) {
        if (++spins < 1024) {
            cpu_relax();
        } else {
            std::this_thread::yield();
            spins = 0;
        }
    }
    completed_base_ = target;
    assert((in_run_.store(false, std::memory_order_relaxed), true));
    cold_transition_gate_.store(false, std::memory_order_release);
}

void GraphRuntimeWorkerPool::run_range(std::uint32_t worker_index) noexcept {
    pulp::signal::ScopedFlushDenormals flush_denormals;
    const Range r = range_for(worker_index, worker_count_, task_count_);
    for (std::uint32_t i = r.begin; i < r.end; ++i) {
        fn_(context_, i);
    }
    // +1 per participant (see run()): the barrier counts participants finished,
    // so even an empty range must register that this worker is done reading the
    // published batch and writing its tasks.
    completed_.fetch_add(1, std::memory_order_acq_rel);
}

void GraphRuntimeWorkerPool::clear_transient_reheat_if_no_worker_cold() noexcept {
    const auto expected_active = worker_count_ > 0 ? worker_count_ - 1 : 0;
    if (active_worker_threads_.load(std::memory_order_acquire) >= expected_active) {
        reheat_requested_.store(false, std::memory_order_release);
    }
}

void GraphRuntimeWorkerPool::worker_loop(std::uint32_t worker_index) noexcept {
    pulp::audio::AudioWorkgroup workgroup;
#if defined(__APPLE__)
    if (void* handle = audio_workgroup_.load(std::memory_order_acquire)) {
        workgroup.set_workgroup(reinterpret_cast<os_workgroup_t>(handle));
    }
#endif
    (void)workgroup.join_from_audio_thread();

    const auto mark_active = [this] {
        const auto active =
            active_worker_threads_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (active >= worker_count_ - 1) {
            reheat_requested_.store(false, std::memory_order_release);
        }
    };

    mark_active();
    std::uint64_t local_epoch = 0;
    auto hot_until = std::chrono::steady_clock::now() + kHotIdleWindow;
    for (;;) {
        std::uint32_t spins = 0;
        std::uint64_t e;
        while ((e = epoch_.load(std::memory_order_acquire)) == local_epoch) {
            if (stopping_.load(std::memory_order_acquire)) return;
            if (++spins < kIdleSpinBeforeBackoff) {
                cpu_relax();
            } else {
                worker_backoff_count_.fetch_add(1, std::memory_order_relaxed);
                spins = 0;
                const auto now = std::chrono::steady_clock::now();
                if (now < hot_until ||
                    reheat_requested_.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                } else {
                    bool gate_expected = false;
                    if (!cold_transition_gate_.compare_exchange_strong(
                            gate_expected,
                            true,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        std::this_thread::yield();
                        continue;
                    }
                    const auto observed_epoch = local_epoch;
                    const bool reheat_requested =
                        reheat_requested_.load(std::memory_order_acquire);
                    if (reheat_requested ||
                        epoch_.load(std::memory_order_acquire) != observed_epoch) {
                        if (reheat_requested) clear_transient_reheat_if_no_worker_cold();
                        cold_transition_gate_.store(false, std::memory_order_release);
                        hot_until = std::chrono::steady_clock::now() + kHotIdleWindow;
                        continue;
                    }
                    active_worker_threads_.fetch_sub(1, std::memory_order_acq_rel);
                    cold_transition_gate_.store(false, std::memory_order_release);
                    do {
                        worker_idle_sleep_count_.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::sleep_for(kColdIdleSleep);
                        if (stopping_.load(std::memory_order_acquire)) return;
                    } while (!reheat_requested_.load(std::memory_order_acquire) &&
                             epoch_.load(std::memory_order_acquire) == observed_epoch);
                    mark_active();
                    hot_until = std::chrono::steady_clock::now() + kHotIdleWindow;
                }
            }
        }
        local_epoch = e;
        if (stopping_.load(std::memory_order_acquire)) return;
        run_range(worker_index);
        hot_until = std::chrono::steady_clock::now() + kHotIdleWindow;
    }
}

} // namespace pulp::format
