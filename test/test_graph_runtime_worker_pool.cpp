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

#if defined(__APPLE__)
#include <os/object.h>
#include <os/workgroup.h>
#endif

namespace {

using pulp::format::GraphRuntimeWorkerPool;

struct SquareCtx {
    std::vector<std::uint32_t> out;
    std::atomic<std::uint32_t> runs{0};
};

struct CallingThreadCtx {
    std::thread::id caller;
    std::atomic<std::uint32_t> runs{0};
    std::atomic<bool> escaped{false};
};

struct FallbackJoinProbe {
    std::atomic<std::uint32_t> attempts{0};
    std::atomic<bool> succeed{true};
};

bool fallback_join_probe(void* context) noexcept {
    auto* probe = static_cast<FallbackJoinProbe*>(context);
    probe->attempts.fetch_add(1, std::memory_order_relaxed);
    return probe->succeed.load(std::memory_order_relaxed);
}

void calling_thread_task(void* ctx, std::uint32_t) noexcept {
    auto* c = static_cast<CallingThreadCtx*>(ctx);
    if (std::this_thread::get_id() != c->caller) {
        c->escaped.store(true, std::memory_order_relaxed);
    }
    c->runs.fetch_add(1, std::memory_order_relaxed);
}

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

bool wait_for_worker_idle_sleep(GraphRuntimeWorkerPool& pool) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pool.worker_idle_sleep_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return pool.worker_idle_sleep_count() > 0;
}

bool wait_for_workgroup_update(GraphRuntimeWorkerPool& pool) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!pool.workers_use_current_audio_workgroup() &&
           std::chrono::steady_clock::now() < deadline) {
        (void)pool.prepare_audio_workgroup_for_render();
        std::this_thread::yield();
    }
    return pool.workers_use_current_audio_workgroup();
}

void* make_test_workgroup(const char* name) {
#if defined(__APPLE__)
    return os_workgroup_parallel_create(name, nullptr);
#else
    (void)name;
    static std::uintptr_t next = 0x1010;
    auto* workgroup = reinterpret_cast<void*>(next);
    next += 0x1010;
    return workgroup;
#endif
}

void release_test_workgroup(void* workgroup) {
#if defined(__APPLE__)
    os_release(workgroup);
#else
    (void)workgroup;
#endif
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

TEST_CASE("WorkerPool cold-idles workers without blocking later batches",
          "[format][worker-pool][rt-safety]") {
    GraphRuntimeWorkerPool pool;
    pool.set_audio_workgroup(nullptr);
    REQUIRE(pool.start(4));
    REQUIRE(wait_for_worker_idle_sleep(pool));

    SquareCtx ctx;
    ctx.out.assign(128, 0xFFFFFFFFu);
    pool.run(128, square_task, &ctx);
    REQUIRE(ctx.runs.load() == 128);
    for (std::uint32_t i = 0; i < 128; ++i) REQUIRE(ctx.out[i] == i * i);
    pool.stop();
}

TEST_CASE("WorkerPool applies workgroup changes and removal to live workers",
          "[format][worker-pool][workgroup][rt-safety]") {
    GraphRuntimeWorkerPool pool;
    auto* first = make_test_workgroup("pulp-worker-pool-first");
    auto* second = make_test_workgroup("pulp-worker-pool-second");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    pool.set_audio_workgroup(first);
    REQUIRE(pool.configured_audio_workgroup() == first);
    REQUIRE(pool.start(4));
    REQUIRE(wait_for_workgroup_update(pool));

    pool.set_audio_workgroup(second);
    REQUIRE(pool.configured_audio_workgroup() == second);
    REQUIRE(wait_for_workgroup_update(pool));

    pool.set_audio_workgroup(nullptr);
    REQUIRE(pool.configured_audio_workgroup() == nullptr);
    pool.wait_for_audio_workgroup_update();
    REQUIRE(pool.workers_use_current_audio_workgroup());
    release_test_workgroup(first);
    release_test_workgroup(second);
    pool.stop();
}

TEST_CASE("WorkerPool distinguishes device fallback from AU context removal",
          "[format][worker-pool][workgroup][rt-safety][lifetime]") {
    GraphRuntimeWorkerPool pool;
    FallbackJoinProbe fallback;
    pool.set_fallback_join_hook_for_test(fallback_join_probe, &fallback);

    // The initial no-handle generation is the same state as a standalone
    // device which cannot provide an OS workgroup: preserve baseline fallback.
    REQUIRE(pool.configured_audio_workgroup() == nullptr);
    REQUIRE(pool.configured_audio_workgroup_uses_fallback_for_test());
    REQUIRE(pool.start(4));
    REQUIRE(pool.prepare_audio_workgroup_for_render());
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 3);

    // AU null means the host removed its realtime render context. Every worker
    // leaves and acknowledges it, but no sticky fallback priority is applied.
    pool.set_audio_workgroup_from_render_context(nullptr);
    REQUIRE_FALSE(pool.configured_audio_workgroup_uses_fallback_for_test());
    REQUIRE(pool.prepare_audio_workgroup_for_render());
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 3);

    // The same null pointer through the standalone/device surface is a distinct
    // generation whose policy requires a real fallback attempt on each worker.
    pool.set_audio_workgroup(nullptr);
    REQUIRE(pool.configured_audio_workgroup_uses_fallback_for_test());
    REQUIRE(pool.prepare_audio_workgroup_for_render());
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 6);
    pool.stop();
}

TEST_CASE("WorkerPool failed device fallback stays inline until republished",
          "[format][worker-pool][workgroup][rt-safety][lifetime]") {
    GraphRuntimeWorkerPool pool;
    FallbackJoinProbe fallback;
    fallback.succeed.store(false, std::memory_order_relaxed);
    pool.set_fallback_join_hook_for_test(fallback_join_probe, &fallback);
    REQUIRE(pool.start(4));

    REQUIRE_FALSE(pool.prepare_audio_workgroup_for_render());
    REQUIRE_FALSE(pool.workers_use_current_audio_workgroup());
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 3);

    CallingThreadCtx first{std::this_thread::get_id()};
    pool.run(64, calling_thread_task, &first);
    REQUIRE(first.runs.load(std::memory_order_relaxed) == 64);
    REQUIRE_FALSE(first.escaped.load(std::memory_order_relaxed));
    // The failed generation is acknowledged and never hot-spin retried.
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 3);

    CallingThreadCtx second{std::this_thread::get_id()};
    pool.run(64, calling_thread_task, &second);
    REQUIRE(second.runs.load(std::memory_order_relaxed) == 64);
    REQUIRE_FALSE(second.escaped.load(std::memory_order_relaxed));
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 3);

    // A later device publication is the retry boundary.
    fallback.succeed.store(true, std::memory_order_relaxed);
    pool.set_audio_workgroup(nullptr);
    REQUIRE(pool.prepare_audio_workgroup_for_render());
    REQUIRE(pool.workers_use_current_audio_workgroup());
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 6);
    pool.stop();
}

TEST_CASE("WorkerPool rapid null publications keep policy coherent",
          "[format][worker-pool][workgroup][rt-safety][lifetime]") {
    GraphRuntimeWorkerPool pool;
    FallbackJoinProbe fallback;
    pool.set_fallback_join_hook_for_test(fallback_join_probe, &fallback);
    REQUIRE(pool.start(4));
    REQUIRE(pool.prepare_audio_workgroup_for_render());
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 3);

    // Pointer ABA cannot expose device policy from N with AU generation N+1.
    {
        pulp::test::RtAllocationProbe allocation_probe;
        for (int i = 0; i < 10000; ++i) {
            pool.set_audio_workgroup(nullptr);
            pool.set_audio_workgroup_from_render_context(nullptr);
        }
        CHECK_FALSE(allocation_probe.saw_allocation());
    }
    REQUIRE_FALSE(pool.configured_audio_workgroup_uses_fallback_for_test());
    REQUIRE(pool.prepare_audio_workgroup_for_render());
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 3);

    // Reverse the final publication. The pointer is still null, but each worker
    // must observe the final device policy and make exactly one fallback attempt.
    {
        pulp::test::RtAllocationProbe allocation_probe;
        for (int i = 0; i < 10000; ++i) {
            pool.set_audio_workgroup_from_render_context(nullptr);
            pool.set_audio_workgroup(nullptr);
        }
        CHECK_FALSE(allocation_probe.saw_allocation());
    }
    REQUIRE(pool.configured_audio_workgroup_uses_fallback_for_test());
    REQUIRE(pool.prepare_audio_workgroup_for_render());
    REQUIRE(fallback.attempts.load(std::memory_order_relaxed) == 6);
    pool.stop();
}

TEST_CASE("WorkerPool rejects a failed non-null workgroup join",
          "[format][worker-pool][workgroup][rt-safety]") {
#if defined(__APPLE__)
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    auto* cancelled = make_test_workgroup("pulp-worker-pool-cancelled");
    REQUIRE(cancelled != nullptr);
    os_workgroup_cancel(reinterpret_cast<os_workgroup_t>(cancelled));

    pool.set_audio_workgroup(cancelled);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    bool falsely_acknowledged = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pool.workers_use_current_audio_workgroup()) {
            falsely_acknowledged = true;
            break;
        }
        std::this_thread::yield();
    }
    REQUIRE_FALSE(falsely_acknowledged);

    // A failed join acknowledges leave(old) but records unusable membership.
    // run() therefore executes inline without retrying this same publication.
    SquareCtx ctx;
    ctx.out.assign(128, 0xFFFFFFFFu);
    pool.run(128, square_task, &ctx);
    REQUIRE(ctx.runs.load() == 128);
    for (std::uint32_t i = 0; i < 128; ++i) REQUIRE(ctx.out[i] == i * i);
    REQUIRE_FALSE(pool.workers_use_current_audio_workgroup());

    pool.set_audio_workgroup(nullptr);
    pool.wait_for_audio_workgroup_update();
    release_test_workgroup(cancelled);
    pool.stop();
#else
    SUCCEED("Borrowed os_workgroup join failure is Apple-only");
#endif
}

TEST_CASE("WorkerPool workgroup publication is allocation-free and preserves batches",
          "[format][worker-pool][workgroup][rt-safety]") {
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    REQUIRE(wait_for_workgroup_update(pool));
    auto* workgroup = make_test_workgroup("pulp-worker-pool-rt");
    REQUIRE(workgroup != nullptr);

    SquareCtx ctx;
    ctx.out.assign(128, 0xFFFFFFFFu);
    {
        pulp::test::RtAllocationProbe probe;
        pool.set_audio_workgroup(workgroup);
        pool.run(128, square_task, &ctx);
        CHECK_FALSE(probe.saw_allocation());
    }
    REQUIRE(ctx.runs.load() == 128);
    for (std::uint32_t i = 0; i < 128; ++i) REQUIRE(ctx.out[i] == i * i);
    REQUIRE(wait_for_workgroup_update(pool));
    pool.set_audio_workgroup(nullptr);
    REQUIRE(wait_for_workgroup_update(pool));
    pool.stop();
    release_test_workgroup(workgroup);
}

TEST_CASE("WorkerPool rapid workgroup publications stay coherent and retire safely",
          "[format][worker-pool][workgroup][rt-safety][lifetime]") {
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    auto* first = make_test_workgroup("pulp-worker-pool-rapid-first");
    auto* second = make_test_workgroup("pulp-worker-pool-rapid-second");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    // Include null and pointer ABA. A worker may coalesce intermediate
    // publications, but must never acknowledge a generation paired with a
    // different handle. Keeping both host-owned handles alive models Apple's
    // render-context lifetime until the final null-removal acknowledgment.
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 10000; ++i) {
            pool.set_audio_workgroup(first);
            pool.set_audio_workgroup(second);
            pool.set_audio_workgroup(nullptr);
            pool.set_audio_workgroup(first);
        }
        CHECK_FALSE(probe.saw_allocation());
    }

    REQUIRE(pool.configured_audio_workgroup() == first);
    REQUIRE(wait_for_workgroup_update(pool));
    pool.set_audio_workgroup(nullptr);
    pool.wait_for_audio_workgroup_update();
    REQUIRE(pool.configured_audio_workgroup() == nullptr);
    REQUIRE(pool.workers_use_current_audio_workgroup());
    pool.stop();
    release_test_workgroup(first);
    release_test_workgroup(second);
}

TEST_CASE("WorkerPool cold workgroup preparation transitions every worker",
          "[format][worker-pool][workgroup][rt-safety][lifetime]") {
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    REQUIRE(wait_for_worker_idle_sleep(pool));
    auto* workgroup = make_test_workgroup("pulp-worker-pool-cold-prepare");
    REQUIRE(workgroup != nullptr);
    pool.set_audio_workgroup(workgroup);

    // A cold sleeper may still be joined to the preceding borrowed AU context.
    // Preparation therefore wakes and waits for every participant instead of
    // excluding it from a finite recruitment window.
    REQUIRE(pool.prepare_audio_workgroup_for_render());
    REQUIRE(pool.workers_use_current_audio_workgroup());

    SquareCtx ctx;
    ctx.out.assign(64, 0xFFFFFFFFu);
    pool.run(64, square_task, &ctx);
    REQUIRE(ctx.runs.load() == 64);
    for (std::uint32_t i = 0; i < 64; ++i) REQUIRE(ctx.out[i] == i * i);
    pool.set_audio_workgroup(nullptr);
    pool.wait_for_audio_workgroup_update();
    pool.stop();
    release_test_workgroup(workgroup);
}

TEST_CASE("WorkerPool AU borrowed contexts transition synchronously before retirement",
          "[format][worker-pool][workgroup][auv3][rt-safety][lifetime]") {
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    auto* first = make_test_workgroup("pulp-worker-pool-au-borrowed-first");
    auto* second = make_test_workgroup("pulp-worker-pool-au-borrowed-second");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    pool.set_audio_workgroup_from_render_context(first);
    REQUIRE(pool.configured_audio_workgroup() == first);
    REQUIRE(wait_for_workgroup_update(pool));

    CallingThreadCtx first_render{std::this_thread::get_id()};
    pool.run(512, calling_thread_task, &first_render);
    REQUIRE(first_render.runs.load(std::memory_order_relaxed) == 512);

    // Observer B is serialized after render A returned. Its triggering render
    // synchronously completes every worker's leave(A)/join(B), including cold
    // sleepers, before A can retire.
    pool.set_audio_workgroup_from_render_context(second);
    REQUIRE(pool.configured_audio_workgroup() == second);
    REQUIRE(wait_for_workgroup_update(pool));
    release_test_workgroup(first);

    CallingThreadCtx second_render{std::this_thread::get_id()};
    pool.run(512, calling_thread_task, &second_render);
    REQUIRE(second_render.runs.load(std::memory_order_relaxed) == 512);

    pool.set_audio_workgroup_from_render_context(nullptr);
    REQUIRE(pool.configured_audio_workgroup() == nullptr);
    REQUIRE(wait_for_workgroup_update(pool));
    pool.stop();
    release_test_workgroup(second);
}

TEST_CASE("WorkerPool transition waits for every delayed worker before retirement",
          "[format][worker-pool][workgroup][rt-safety][lifetime][threads]") {
#ifndef NDEBUG
    GraphRuntimeWorkerPool pool;
    REQUIRE(pool.start(4));
    auto* workgroup = make_test_workgroup("pulp-worker-pool-paused");
    REQUIRE(workgroup != nullptr);

    const auto wait_for_pause = [&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!pool.workgroup_transition_pause_reached_for_test() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return pool.workgroup_transition_pause_reached_for_test();
    };

    // Pin workers immediately before adoption. The render barrier must remain
    // outstanding; returning here would let the host retire their old context.
    pool.set_audio_workgroup_from_render_context(workgroup);
    pool.pause_workgroup_transition_for_test();
    std::atomic<bool> render_done{false};
    std::thread render([&] {
        (void)pool.prepare_audio_workgroup_for_render();
        render_done.store(true, std::memory_order_release);
    });
    REQUIRE(wait_for_pause());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE_FALSE(render_done.load(std::memory_order_acquire));
    pool.release_workgroup_transition_for_test();
    render.join();
    REQUIRE(render_done.load(std::memory_order_acquire));
    pool.clear_workgroup_transition_pause_for_test();
    REQUIRE(pool.workers_use_current_audio_workgroup());

    pool.set_audio_workgroup_from_render_context(nullptr);
    pool.wait_for_audio_workgroup_update();
    pool.stop();
    release_test_workgroup(workgroup);
#else
    SUCCEED("Deterministic transition pause hooks are Debug-only");
#endif
}

TEST_CASE("WorkerPool clears a gate-collision reheat only when no worker parked",
          "[format][worker-pool][rt-safety]") {
    GraphRuntimeWorkerPool pool;

    pool.seed_reheat_state_for_test(/*worker_count=*/4, /*active_worker_threads=*/3,
                                    /*reheat_requested=*/true);
    pool.clear_transient_reheat_if_no_worker_cold_for_test();
    CHECK_FALSE(pool.reheat_requested_for_test());

    pool.seed_reheat_state_for_test(/*worker_count=*/4, /*active_worker_threads=*/2,
                                    /*reheat_requested=*/true);
    pool.clear_transient_reheat_if_no_worker_cold_for_test();
    CHECK(pool.reheat_requested_for_test());
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
