#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace pulp::format {

/// Persistent fork-join worker pool for the levelized parallel graph executor.
///
/// Threads are spawned once at start() (off the audio thread). After a batch
/// they stay hot briefly with a bounded spin/yield loop, then fall back to short
/// cold-idle sleeps. If a later run() arrives while workers are cold, that batch
/// runs inline and requests worker reheating for subsequent batches. run() executes
/// `task_count` tasks across the pool's threads PLUS the calling (audio) thread,
/// then returns when every participant has completed — no allocation, no lock, no
/// condition variable, and no OS wakeup from run(). The audio callback owns the
/// fork-join; workers never allocate or steal.
///
/// Tasks are claimed cooperatively via an atomic cursor, so a task is a small
/// index the caller maps to a unit of work (e.g. a node in the current level).
/// The same pool is reused across blocks. Worker threads enter the audio
/// workgroup or best-effort realtime priority path once, then keep denormals
/// flushed while running every batch.
///
/// Lifetime: start()/stop() are control-thread only. run() is audio-thread only
/// and must not overlap a stop(). A run_fn must be RT-safe (no alloc/lock) — the
/// pool only provides the fork-join, not RT-safety of the work itself.
class GraphRuntimeWorkerPool {
public:
    using TaskFn = void (*)(void* context, std::uint32_t task_index) noexcept;

    GraphRuntimeWorkerPool() = default;
    ~GraphRuntimeWorkerPool();

    GraphRuntimeWorkerPool(const GraphRuntimeWorkerPool&) = delete;
    GraphRuntimeWorkerPool& operator=(const GraphRuntimeWorkerPool&) = delete;

    // Off-RT: spawn `worker_count` total participants (worker_count - 1 threads;
    // the caller of run() is the worker_count-th). worker_count <= 1 means run()
    // executes everything inline on the caller with no threads. Re-start() after
    // stop(). Returns false on an invalid count or thread-spawn failure.
    bool start(std::uint32_t worker_count);

    // Off-RT: signal and join all worker threads. Idempotent.
    void stop() noexcept;

    // RT-safe: publish an optional platform audio workgroup handle. May be
    // called before start() or when the render context changes. Each worker
    // leaves its old group and joins the new one on its own thread before it
    // participates in another batch. A null device handle preserves the
    // best-effort realtime-priority fallback used when no OS workgroup exists.
    void set_audio_workgroup(void* workgroup) noexcept;

    // AU observer path: coherent O(1) publication. The raw workgroup is borrowed
    // and is adopted by every worker in the synchronous triggering-render
    // barrier before the host may retire the preceding context.
    void set_audio_workgroup_from_render_context(void* workgroup) noexcept;

    // RT-safe full-participant transition barrier. Every worker leaves its old
    // context and acknowledges the new publication before this returns, even
    // when it was cold. False means at least one new join failed and subsequent
    // graph work must remain inline for this publication.
    bool prepare_audio_workgroup_for_render() noexcept;

    // Off-RT: wait until every worker has left/joined the last publication.
    // Used before an owner invalidates a borrowed workgroup handle.
    void wait_for_audio_workgroup_update() noexcept;

    void* configured_audio_workgroup() const noexcept {
        return current_audio_workgroup_publication().workgroup;
    }
    bool workers_use_current_audio_workgroup() const noexcept;

    std::uint32_t worker_count() const noexcept { return worker_count_; }
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    std::uint64_t worker_backoff_count() const noexcept {
        return worker_backoff_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t worker_idle_sleep_count() const noexcept {
        return worker_idle_sleep_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t worker_park_count() const noexcept {
        return worker_idle_sleep_count();
    }

#ifdef PULP_GRAPH_RUNTIME_WORKER_POOL_TEST_HOOKS
    // Install before start(). This intercepts only the no-handle fallback join,
    // allowing tests to prove that device-null attempts fallback while AU-null
    // removal does not. Production workgroup joins are never redirected.
    void set_fallback_join_hook_for_test(bool (*hook)(void*) noexcept,
                                         void* context) noexcept {
        assert(!running_.load(std::memory_order_acquire));
        fallback_join_hook_for_test_ = hook;
        fallback_join_context_for_test_ = context;
    }
    bool configured_audio_workgroup_uses_fallback_for_test() const noexcept {
        return current_audio_workgroup_publication().fallback_when_null;
    }
#ifndef NDEBUG
    void pause_workgroup_transition_for_test() noexcept {
        transition_pause_released_for_test_.store(false, std::memory_order_release);
        transition_pause_reached_for_test_.store(false, std::memory_order_release);
        transition_pause_enabled_for_test_.store(true, std::memory_order_release);
    }
    bool workgroup_transition_pause_reached_for_test() const noexcept {
        return transition_pause_reached_for_test_.load(std::memory_order_acquire);
    }
    void release_workgroup_transition_for_test() noexcept {
        transition_pause_released_for_test_.store(true, std::memory_order_release);
    }
    void clear_workgroup_transition_pause_for_test() noexcept {
        transition_pause_enabled_for_test_.store(false, std::memory_order_release);
    }
#endif
    void seed_reheat_state_for_test(std::uint32_t worker_count,
                                    std::uint32_t active_worker_threads,
                                    bool reheat_requested) noexcept {
        worker_count_ = worker_count;
        active_worker_threads_.store(active_worker_threads, std::memory_order_release);
        reheat_requested_.store(reheat_requested, std::memory_order_release);
    }
    void clear_transient_reheat_if_no_worker_cold_for_test() noexcept {
        clear_transient_reheat_if_no_worker_cold();
    }
    bool reheat_requested_for_test() const noexcept {
        return reheat_requested_.load(std::memory_order_acquire);
    }
#endif

    // RT-safe: run `fn(context, i)` for every i in [0, task_count) exactly once,
    // distributed across the pool threads and the calling thread, and return only
    // after all have completed. Allocation-free and lock-free. With no worker
    // threads (worker_count <= 1) it simply runs them inline on the caller.
    void run(std::uint32_t task_count, TaskFn fn, void* context) noexcept;

private:
    struct AudioWorkgroupPublication {
        void* workgroup = nullptr;
        std::uint64_t generation = 0;
        bool fallback_when_null = true;
    };

    AudioWorkgroupPublication current_audio_workgroup_publication() const noexcept;
    bool workers_acknowledged_current_audio_workgroup() const noexcept;
    void publish_audio_workgroup(void* workgroup,
                                 bool fallback_when_null) noexcept;
    void worker_loop(std::uint32_t worker_index) noexcept;
    void clear_transient_reheat_if_no_worker_cold() noexcept;
    // Run this worker's STATIC contiguous task range for the current batch and
    // add its size to the completion counter. Static partitioning (no shared
    // claim cursor) means a lagging worker can never re-claim tasks after run()
    // publishes the next batch — run() returns only once completed_ == task_count
    // (every worker's add is done), so the next batch's counter reset is safe.
    void run_range(std::uint32_t worker_index) noexcept;

    std::vector<std::thread> threads_;
    std::uint32_t worker_count_ = 0;
    // Single-writer seqlock publication. AU's render-context observer is called
    // serially on the render thread; standalone device changes are serialized
    // by CoreAudioDevice::switch_mutex_. An odd sequence means the pointer is
    // being changed, and an even sequence identifies one immutable snapshot.
    // Workers never combine a pointer or null policy from publication N with
    // N +/- 1's generation, including under rapid A -> B -> null -> A changes.
    std::atomic<std::uint64_t> audio_workgroup_sequence_{2};
    std::atomic<void*> audio_workgroup_{nullptr};
    std::atomic<bool> audio_workgroup_fallback_when_null_{true};
    std::unique_ptr<std::atomic<std::uint64_t>[]> worker_workgroup_generation_;
    std::unique_ptr<std::atomic<bool>[]> worker_workgroup_join_succeeded_;

    // Immutable while workers are running. The setter is exposed only to the
    // focused test target, but these fields remain in every build so the class
    // layout cannot differ between the library and its consumer.
    bool (*fallback_join_hook_for_test_)(void*) noexcept = nullptr;
    void* fallback_join_context_for_test_ = nullptr;

    // Published batch (valid for the current epoch). Written by run() before the
    // epoch bump (release), read by workers after observing the new epoch
    // (acquire). run() must not notify parked OS waiters. If not every worker is
    // hot, run() executes inline and lets sleepers reheat on their next cold-idle
    // tick.
    TaskFn fn_ = nullptr;
    void* context_ = nullptr;
    std::uint32_t task_count_ = 0;

    // Epoch a worker polls; bumped by run() to publish a batch. stop() bumps it
    // with `stopping_` set so workers exit.
    alignas(64) std::atomic<std::uint64_t> epoch_{0};
    alignas(64) std::atomic<bool> stopping_{false};
    alignas(64) std::atomic<bool> running_{false};
    alignas(64) std::atomic<bool> cold_transition_gate_{false};
    alignas(64) std::atomic<bool> reheat_requested_{false};
    // A workgroup transition uses a tagged epoch distinct from task epochs.
    // Every worker, including a cold sleeper, must observe and acknowledge it;
    // this is what lets an AU host retire the preceding borrowed context when
    // the triggering render returns.
    alignas(64) std::atomic<std::uint64_t> workgroup_prepare_epoch_{0};
    alignas(64) std::atomic<std::uint32_t> active_worker_threads_{0};

    // MONOTONIC count of tasks completed across all batches (each worker adds its
    // range size with release; the caller acquire-waits until it reaches the
    // batch target). Never reset — a reset would race a lagging worker's add from
    // the previous batch. 64-bit so it never wraps in a long session.
    alignas(64) std::atomic<std::uint64_t> completed_{0};
    // The value of completed_ before the current batch (caller-only scratch, so
    // the per-batch target is completed_base_ + task_count_).
    std::uint64_t completed_base_ = 0;

    // Debug-only guard asserting run() is never called re-entrantly / from two
    // threads at once — the load-bearing invariant the lock-free barrier relies
    // on. Compiled out (and never touched) in release builds.
    alignas(64) std::atomic<bool> in_run_{false};
    alignas(64) std::atomic<std::uint64_t> worker_backoff_count_{0};
    alignas(64) std::atomic<std::uint64_t> worker_idle_sleep_count_{0};
#ifndef NDEBUG
    // Deterministic race-test seam, absent from Release builds and checked only
    // on the rare workgroup-transition path.
    std::atomic<bool> transition_pause_enabled_for_test_{false};
    std::atomic<bool> transition_pause_reached_for_test_{false};
    std::atomic<bool> transition_pause_released_for_test_{true};
#endif
};

} // namespace pulp::format
