#pragma once

#include <atomic>
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
    // participates in another batch. Null removes the current group.
    void set_audio_workgroup(void* workgroup) noexcept;

    // Off-RT: wait until every worker has left/joined the last publication.
    // Used before an owner invalidates a borrowed workgroup handle.
    void wait_for_audio_workgroup_update() noexcept;

    void* configured_audio_workgroup() const noexcept {
        return audio_workgroup_.load(std::memory_order_acquire);
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
    std::atomic<void*> audio_workgroup_{nullptr};
    std::atomic<std::uint64_t> audio_workgroup_generation_{1};
    std::unique_ptr<std::atomic<std::uint64_t>[]> worker_workgroup_generation_;

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
};

} // namespace pulp::format
