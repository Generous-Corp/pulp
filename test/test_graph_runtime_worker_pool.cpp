// Coverage for the persistent fork-join worker pool the levelized parallel
// executor uses. Verifies every task in a batch runs exactly once, results are
// visible to the caller after run() returns, batches reuse the pool without
// races (run under TSan in CI), and the inline (no-thread) path works.

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/graph_runtime_worker_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

namespace {

using pulp::format::GraphRuntimeWorkerPool;

struct SquareCtx {
    std::vector<std::uint32_t> out;
    std::atomic<std::uint32_t> runs{0};
};

// Each task writes its own slot (disjoint — the executor's model) and bumps a
// shared run counter so a double-run or missed-run is detectable.
void square_task(void* ctx, std::uint32_t i) noexcept {
    auto* c = static_cast<SquareCtx*>(ctx);
    c->out[i] = i * i;
    c->runs.fetch_add(1, std::memory_order_relaxed);
}

bool wait_for_progress(const std::atomic<std::uint64_t>& counter) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (counter.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return counter.load(std::memory_order_relaxed) > 0;
}

} // namespace

TEST_CASE("WorkerPool runs every task in a batch exactly once",
          "[format][worker-pool]") {
    for (std::uint32_t workers : {1u, 2u, 4u, 8u}) {
        GraphRuntimeWorkerPool pool;
        REQUIRE(pool.start(workers));
        REQUIRE(pool.worker_count() == workers);

        constexpr std::uint32_t kTasks = 1000;
        SquareCtx ctx;
        ctx.out.assign(kTasks, 0xFFFFFFFFu);
        pool.run(kTasks, square_task, &ctx);

        CHECK(ctx.runs.load() == kTasks);  // each task ran exactly once
        for (std::uint32_t i = 0; i < kTasks; ++i) {
            CHECK(ctx.out[i] == i * i);  // results visible after run() returns
        }
        pool.stop();
    }
}

TEST_CASE("WorkerPool reuses the pool across many batches without races",
          "[format][worker-pool]") {
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    // Smaller and larger batches, including counts below the worker count and
    // odd counts that produce uneven static ranges.
    for (std::uint32_t batch = 0; batch < 200; ++batch) {
        const std::uint32_t kTasks = (batch % 7) + 1 + (batch % 13) * 9;
        SquareCtx ctx;
        ctx.out.assign(kTasks, 0xFFFFFFFFu);
        pool.run(kTasks, square_task, &ctx);
        REQUIRE(ctx.runs.load() == kTasks);
        for (std::uint32_t i = 0; i < kTasks; ++i) REQUIRE(ctx.out[i] == i * i);
    }
    pool.stop();
}

TEST_CASE("WorkerPool with a single participant runs inline",
          "[format][worker-pool]") {
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(1));  // caller is the only participant; no threads
    SquareCtx ctx;
    ctx.out.assign(64, 0);
    pool.run(64, square_task, &ctx);
    CHECK(ctx.runs.load() == 64);
    for (std::uint32_t i = 0; i < 64; ++i) CHECK(ctx.out[i] == i * i);
    pool.stop();
}

TEST_CASE("WorkerPool run() is allocation-free on the calling thread",
          "[format][worker-pool][rt-safety]") {
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    SquareCtx ctx;
    ctx.out.assign(256, 0);
    pool.run(256, square_task, &ctx);  // warm up the threads outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        pool.run(256, square_task, &ctx);
        CHECK_FALSE(probe.saw_allocation());
    }
    pool.stop();
}

TEST_CASE("WorkerPool run() with zero tasks is a no-op",
          "[format][worker-pool]") {
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    SquareCtx ctx;
    ctx.out.assign(4, 7);
    pool.run(0, square_task, &ctx);
    CHECK(ctx.runs.load() == 0);
    pool.stop();
}

TEST_CASE("WorkerPool oversubscribes more workers than cores",
          "[format][worker-pool]") {
    // Far more participants than hardware cores stresses the yield-backoff slow
    // path and oversubscription, where a lost wakeup or starvation would surface.
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(64));
    for (std::uint32_t batch = 0; batch < 20; ++batch) {
        SquareCtx ctx;
        ctx.out.assign(500, 0xFFFFFFFFu);
        pool.run(500, square_task, &ctx);
        REQUIRE(ctx.runs.load() == 500);
        for (std::uint32_t i = 0; i < 500; ++i) REQUIRE(ctx.out[i] == i * i);
    }
    pool.stop();
}

TEST_CASE("WorkerPool restarts cleanly after stop",
          "[format][worker-pool]") {
    GraphRuntimeWorkerPool pool;
    for (int cycle = 0; cycle < 3; ++cycle) {
        REQUIRE(pool.start(4));
        SquareCtx ctx;
        ctx.out.assign(300, 0xFFFFFFFFu);
        pool.run(300, square_task, &ctx);
        CHECK(ctx.runs.load() == 300);
        for (std::uint32_t i = 0; i < 300; ++i) CHECK(ctx.out[i] == i * i);
        pool.stop();
    }
}

TEST_CASE("WorkerPool stop is idempotent",
          "[format][worker-pool]") {
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    pool.stop();
    pool.stop();  // second stop is a no-op, not a crash
    CHECK_FALSE(pool.running());
}

TEST_CASE("WorkerPool started on one thread, run() driven from another, is race-free",
          "[format][graph][worker-pool][threads][rt-safety]") {
    // The SignalGraph parallel path starts the pool off the control thread (during
    // prepare) but drives run() from the audio thread. Exercise that exact split:
    // start here, then run() repeatedly from a separate thread while this thread
    // re-validates the pool. Surfaces a first-batch publish race under TSan.
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(8));
    constexpr std::uint32_t kTasks = 500;
    SquareCtx ctx;
    ctx.out.assign(kTasks, 0);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> batches{0};
    std::thread driver([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            ctx.runs.store(0, std::memory_order_relaxed);
            pool.run(kTasks, square_task, &ctx);
            batches.fetch_add(1, std::memory_order_relaxed);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (batches.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        (void)pool.running();
        (void)pool.worker_count();
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    driver.join();
    CHECK(batches.load() > 0);
    for (std::uint32_t i = 0; i < kTasks; ++i) CHECK(ctx.out[i] == i * i);
}

TEST_CASE("WorkerPool rapid back-to-back batches from a separate thread are race-free",
          "[format][graph][worker-pool][threads][rt-safety]") {
    // process_parallel fires several run() calls in quick succession (one per
    // level) within a single audio block; mimic that exactly: start on this
    // thread, then a driver fires 3 tiny back-to-back batches per "block", many
    // times. width(8) < workers(16) leaves participant 0 + even workers with
    // empty ranges, matching the wide-level partition.
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(16));
    SquareCtx ctx;
    ctx.out.assign(8, 0);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> blocks{0};
    std::thread driver([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            pool.run(8, square_task, &ctx);
            pool.run(8, square_task, &ctx);
            pool.run(8, square_task, &ctx);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    CHECK(wait_for_progress(blocks));
    stop.store(true, std::memory_order_relaxed);
    driver.join();
}
