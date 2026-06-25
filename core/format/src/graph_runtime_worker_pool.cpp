#include <pulp/format/graph_runtime_worker_pool.hpp>

#include <cassert>

namespace pulp::format {
namespace {

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

} // namespace

GraphRuntimeWorkerPool::~GraphRuntimeWorkerPool() { stop(); }

bool GraphRuntimeWorkerPool::start(std::uint32_t worker_count) {
    stop();
    if (worker_count == 0) return false;
    worker_count_ = worker_count;
    stopping_.store(false, std::memory_order_release);
    epoch_.store(0, std::memory_order_release);
    completed_.store(0, std::memory_order_release);
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
        // Park-loop workers re-check stopping_ each iteration, so setting it is
        // enough to release them; bump the epoch too so a worker blocked on the
        // epoch wait observes a change immediately.
        stopping_.store(true, std::memory_order_release);
        epoch_.fetch_add(1, std::memory_order_release);
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }
    worker_count_ = 0;
    running_.store(false, std::memory_order_release);
}

void GraphRuntimeWorkerPool::run(std::uint32_t task_count, TaskFn fn,
                                 void* context) noexcept {
    if (task_count == 0 || fn == nullptr) return;
    // No worker threads: run everything inline on the caller.
    if (worker_count_ <= 1 || threads_.empty()) {
        for (std::uint32_t i = 0; i < task_count; ++i) fn(context, i);
        return;
    }

    // run() must not overlap another run() (the lock-free barrier's correctness
    // depends on serialized batches); enforce it loudly in debug builds.
    assert(!in_run_.exchange(true, std::memory_order_relaxed) &&
           "GraphRuntimeWorkerPool::run() is not re-entrant and must not run concurrently");

    // Publish the batch, then release it to the parked workers via the epoch.
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
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ __volatile__("yield");
#endif
        } else {
            std::this_thread::yield();
            spins = 0;
        }
    }
    completed_base_ = target;
    assert((in_run_.store(false, std::memory_order_relaxed), true));
}

void GraphRuntimeWorkerPool::run_range(std::uint32_t worker_index) noexcept {
    const Range r = range_for(worker_index, worker_count_, task_count_);
    for (std::uint32_t i = r.begin; i < r.end; ++i) {
        fn_(context_, i);
    }
    // +1 per participant (see run()): the barrier counts participants finished,
    // so even an empty range must register that this worker is done reading the
    // published batch and writing its tasks.
    completed_.fetch_add(1, std::memory_order_acq_rel);
}

void GraphRuntimeWorkerPool::worker_loop(std::uint32_t worker_index) noexcept {
    std::uint64_t local_epoch = 0;
    for (;;) {
        std::uint32_t spins = 0;
        std::uint64_t e;
        while ((e = epoch_.load(std::memory_order_acquire)) == local_epoch) {
            if (stopping_.load(std::memory_order_acquire)) return;
            if (++spins < 1024) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#elif defined(__aarch64__)
                __asm__ __volatile__("yield");
#endif
            } else {
                std::this_thread::yield();
                spins = 0;
            }
        }
        local_epoch = e;
        if (stopping_.load(std::memory_order_acquire)) return;
        run_range(worker_index);
    }
}

} // namespace pulp::format
