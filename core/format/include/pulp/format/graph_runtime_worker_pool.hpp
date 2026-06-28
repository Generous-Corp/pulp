#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace pulp::format {

/// Persistent fork-join worker pool for the levelized parallel graph executor.
///
/// Threads are spawned once at start() (off the audio thread) and parked on a
/// bounded atomic until run() publishes a batch. run() executes `task_count`
/// tasks across the pool's threads PLUS the calling (audio) thread, then returns
/// when every task has completed — no allocation, no lock, no condition variable
/// in run(). It is the persistent-worker / epoch-publish / bounded-atomic
/// pattern (masterwork §6.1): the audio callback owns the fork-join, workers
/// never allocate or steal.
///
/// Tasks are claimed cooperatively via an atomic cursor, so a task is a small
/// index the caller maps to a unit of work (e.g. a node in the current level).
/// The same pool is reused across blocks. Idle workers spin with a yield-backoff;
/// OS-park / device-workgroup joins are outside the current pool contract.
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

    std::uint32_t worker_count() const noexcept { return worker_count_; }
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    // RT-safe: run `fn(context, i)` for every i in [0, task_count) exactly once,
    // distributed across the pool threads and the calling thread, and return only
    // after all have completed. Allocation-free and lock-free. With no worker
    // threads (worker_count <= 1) it simply runs them inline on the caller.
    void run(std::uint32_t task_count, TaskFn fn, void* context) noexcept;

private:
    void worker_loop(std::uint32_t worker_index) noexcept;
    // Run this worker's STATIC contiguous task range for the current batch and
    // add its size to the completion counter. Static partitioning (no shared
    // claim cursor) means a lagging worker can never re-claim tasks after run()
    // publishes the next batch — run() returns only once completed_ == task_count
    // (every worker's add is done), so the next batch's counter reset is safe.
    void run_range(std::uint32_t worker_index) noexcept;

    std::vector<std::thread> threads_;
    std::uint32_t worker_count_ = 0;

    // Published batch (valid for the current epoch). Written by run() before the
    // epoch bump (release), read by workers after observing the new epoch
    // (acquire).
    TaskFn fn_ = nullptr;
    void* context_ = nullptr;
    std::uint32_t task_count_ = 0;

    // Epoch a worker waits on; bumped by run() to publish a batch. stop() bumps
    // it with `stopping_` set so workers exit.
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};

    // MONOTONIC count of tasks completed across all batches (each worker adds its
    // range size with release; the caller acquire-waits until it reaches the
    // batch target). Never reset — a reset would race a lagging worker's add from
    // the previous batch. 64-bit so it never wraps in a long session.
    std::atomic<std::uint64_t> completed_{0};
    // The value of completed_ before the current batch (caller-only scratch, so
    // the per-batch target is completed_base_ + task_count_).
    std::uint64_t completed_base_ = 0;

    // Debug-only guard asserting run() is never called re-entrantly / from two
    // threads at once — the load-bearing invariant the lock-free barrier relies
    // on. Compiled out (and never touched) in release builds.
    std::atomic<bool> in_run_{false};
};

} // namespace pulp::format
