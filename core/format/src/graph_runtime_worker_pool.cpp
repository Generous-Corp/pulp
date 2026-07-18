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

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "GraphRuntimeWorkerPool RT epochs require lock-free uint64 atomics");
static_assert(std::atomic<void*>::is_always_lock_free,
              "GraphRuntimeWorkerPool RT publication requires lock-free pointers");
static_assert(std::atomic<bool>::is_always_lock_free,
              "GraphRuntimeWorkerPool RT acknowledgments require lock-free bool atomics");

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
    workgroup_prepare_epoch_.store(0, std::memory_order_release);
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
        worker_workgroup_generation_ =
            std::make_unique<std::atomic<std::uint64_t>[]>(worker_count_ - 1);
        worker_workgroup_join_succeeded_ =
            std::make_unique<std::atomic<bool>[]>(worker_count_ - 1);
        for (std::uint32_t w = 1; w < worker_count_; ++w) {
            worker_workgroup_generation_[w - 1].store(0, std::memory_order_relaxed);
            worker_workgroup_join_succeeded_[w - 1].store(
                false, std::memory_order_relaxed);
        }
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
    worker_workgroup_generation_.reset();
    worker_workgroup_join_succeeded_.reset();
    active_worker_threads_.store(0, std::memory_order_release);
    cold_transition_gate_.store(false, std::memory_order_release);
    reheat_requested_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

void GraphRuntimeWorkerPool::set_audio_workgroup(void* workgroup) noexcept {
    // Standalone/CoreAudio path: the device owns a caller-retained query
    // reference until every worker acknowledges removal. This lifetime permits
    // adoption at the next explicit full-participant preparation barrier.
    publish_audio_workgroup(workgroup);
}

void GraphRuntimeWorkerPool::set_audio_workgroup_from_render_context(
    void* workgroup) noexcept {
    // AU observer path. Reference counting is not documented RT-safe.
    // Publication is therefore borrowed and adoption is completed synchronously
    // inside the triggering render before its serialized thread can receive B.
    publish_audio_workgroup(workgroup);
}

void GraphRuntimeWorkerPool::publish_audio_workgroup(void* workgroup) noexcept {
    // Single-writer seqlock. AU observers are serialized on the render thread;
    // CoreAudio device publications are serialized by switch_mutex_ and happen
    // with the device callback stopped. Pointer + generation are one coherent
    // logical publication without allocation, retain/release, or a lock.
    const auto odd = audio_workgroup_sequence_.fetch_add(
                         1, std::memory_order_acq_rel) +
                     1;
    audio_workgroup_.store(workgroup, std::memory_order_relaxed);
    audio_workgroup_sequence_.store(odd + 1, std::memory_order_release);
    // Cold workers wake on their bounded polling interval. Until every worker
    // adopts this generation, run() remains inline rather than dispatching into
    // a stale render context.
    reheat_requested_.store(true, std::memory_order_release);
}

GraphRuntimeWorkerPool::AudioWorkgroupPublication
GraphRuntimeWorkerPool::current_audio_workgroup_publication() const noexcept {
    for (;;) {
        const auto before =
            audio_workgroup_sequence_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) {
            cpu_relax();
            continue;
        }
        void* const workgroup =
            audio_workgroup_.load(std::memory_order_relaxed);
        const auto after =
            audio_workgroup_sequence_.load(std::memory_order_acquire);
        if (before == after) return {workgroup, after};
    }
}

bool GraphRuntimeWorkerPool::workers_use_current_audio_workgroup() const noexcept {
    if (!workers_acknowledged_current_audio_workgroup()) return false;
    if (worker_count_ <= 1 || threads_.empty()) return true;
    for (std::uint32_t w = 1; w < worker_count_; ++w) {
        if (!worker_workgroup_join_succeeded_[w - 1].load(
                std::memory_order_acquire)) {
            return false;
        }
    }
    return true;
}

bool GraphRuntimeWorkerPool::workers_acknowledged_current_audio_workgroup()
    const noexcept {
    if (worker_count_ <= 1 || threads_.empty()) return true;
    if (!worker_workgroup_generation_) return false;
    const auto publication = current_audio_workgroup_publication();
    for (std::uint32_t w = 1; w < worker_count_; ++w) {
        if (worker_workgroup_generation_[w - 1].load(std::memory_order_acquire) !=
            publication.generation) {
            return false;
        }
    }
    return true;
}

void GraphRuntimeWorkerPool::wait_for_audio_workgroup_update() noexcept {
    // Control-thread only. A failed new join still acknowledges the publication:
    // that worker has completed leave(old), so the old owner may retire safely.
    (void)prepare_audio_workgroup_for_render();
}

bool GraphRuntimeWorkerPool::prepare_audio_workgroup_for_render() noexcept {
    if (worker_count_ <= 1 || threads_.empty()) return true;
    if (workers_acknowledged_current_audio_workgroup()) {
        return workers_use_current_audio_workgroup();
    }

    // Serialize with a worker entering its cold sleep and with run(). Unlike a
    // normal batch, a borrowed AU transition cannot fall back when a worker is
    // cold: that worker may still be joined to the context the host is about to
    // retire. This lock-free gate wait and every barrier below use CPU relax
    // only; no scheduler syscall is made from the render thread.
    for (;;) {
        bool gate_expected = false;
        if (cold_transition_gate_.compare_exchange_weak(
                gate_expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
        cpu_relax();
    }
    if (workers_acknowledged_current_audio_workgroup()) {
        const bool joined = workers_use_current_audio_workgroup();
        cold_transition_gate_.store(false, std::memory_order_release);
        return joined;
    }

    const auto publication = current_audio_workgroup_publication();
    // Publish a transition-only epoch, separate from the task completion
    // counter. reheat_requested_ causes cold sleepers to resume on their next
    // bounded poll; every worker must acknowledge this exact generation.
    const auto prepare_epoch = epoch_.load(std::memory_order_relaxed) + 1;
    workgroup_prepare_epoch_.store(prepare_epoch, std::memory_order_relaxed);
    reheat_requested_.store(true, std::memory_order_release);
    epoch_.store(prepare_epoch, std::memory_order_release);

    for (std::uint32_t w = 1; w < worker_count_; ++w) {
        while (worker_workgroup_generation_[w - 1].load(
                   std::memory_order_acquire) != publication.generation) {
            cpu_relax();
        }
    }
    cold_transition_gate_.store(false, std::memory_order_release);
    return workers_use_current_audio_workgroup();
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
    if (!workers_use_current_audio_workgroup() &&
        !prepare_audio_workgroup_for_render()) {
        // Every worker acknowledged this publication, but at least one join
        // failed. Do not hot-spin retries against the same borrowed handle;
        // remain inline until a later observer publication supplies a new one.
        reheat_requested_.store(false, std::memory_order_release);
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
    while (completed_.load(std::memory_order_acquire) < target) {
        cpu_relax();
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
    std::uint64_t local_workgroup_generation = 0;
    const auto adopt_audio_workgroup = [&] {
        const auto publication = current_audio_workgroup_publication();
        if (publication.generation == local_workgroup_generation) return;

        workgroup.leave();
        bool adopted = true;
#if defined(__APPLE__)
        const auto handle = reinterpret_cast<os_workgroup_t>(
            publication.workgroup);
        workgroup.set_workgroup(handle);
        // A null AU render context means leave the old workgroup and join
        // nothing. Do not invoke AudioWorkgroup's standalone fallback here:
        // that would keep an auxiliary worker realtime in a non-realtime host
        // context instead of honoring removal.
        if (handle) adopted = workgroup.join_from_audio_thread();
#else
        // There is no borrowed platform handle to validate off Apple. Keep
        // the best-effort priority attempt, but it cannot make a null-removal
        // generation unsafe to acknowledge.
        (void)workgroup.join_from_audio_thread();
#endif
        // If a newer observer call raced the join, this worker is safely joined
        // to an intermediate host-owned context but must not advertise itself
        // current. Its next poll leaves that group and adopts the latest one.
        // This check also closes rapid publication ABA: equality includes the
        // stable even sequence, not only the recycled pointer value.
        if (current_audio_workgroup_publication().generation !=
            publication.generation) {
            return;
        }
        local_workgroup_generation = publication.generation;
        worker_workgroup_join_succeeded_[worker_index - 1].store(
            adopted, std::memory_order_release);
        worker_workgroup_generation_[worker_index - 1].store(
            publication.generation, std::memory_order_release);
        // start() may still be appending to threads_ while early workers enter
        // this loop. Inspect only the fully allocated generation array here;
        // workers must never read the control-thread-owned vector.
        bool all_workers_current = true;
        for (std::uint32_t w = 1; w < worker_count_; ++w) {
            if (worker_workgroup_generation_[w - 1].load(
                    std::memory_order_acquire) != publication.generation) {
                all_workers_current = false;
                break;
            }
        }
        if (all_workers_current) {
            bool expected = true;
            reheat_requested_.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel,
                std::memory_order_acquire);
        }
    };

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
        if (workgroup_prepare_epoch_.load(std::memory_order_acquire) == e) {
#ifndef NDEBUG
            if (transition_pause_enabled_for_test_.load(
                    std::memory_order_acquire)) {
                transition_pause_reached_for_test_.store(
                    true, std::memory_order_release);
                while (!transition_pause_released_for_test_.load(
                    std::memory_order_acquire)) {
                    cpu_relax();
                }
            }
#endif
            adopt_audio_workgroup();
        } else {
            run_range(worker_index);
        }
        hot_until = std::chrono::steady_clock::now() + kHotIdleWindow;
    }
}

} // namespace pulp::format
