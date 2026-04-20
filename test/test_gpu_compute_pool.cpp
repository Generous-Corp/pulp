// test_gpu_compute_pool.cpp — unit tests for the staging buffer pool
// introduced in Slice 1 of the zero-copy ralph loop.
//
// Design reference: planning/zero-copy-slice-1-design-2026-04-20.md
//
// Tests live here (not in test_gpu_compute.cpp) because they exercise
// the pool in isolation using a real Dawn device obtained from
// GpuCompute's standalone init path, without touching the compute
// pipelines. This keeps them fast and independent of the shader
// round-trip tests.

#include <catch2/catch_test_macros.hpp>

#ifdef PULP_HAS_SKIA

// Pull in the same Dawn headers the implementation uses. The pool
// header is an internal one in core/render/src, so we reach into it
// by relative path — matches how other in-tree tests reach private
// headers (e.g. test_webgpu_surface).
#include "../core/render/src/gpu_compute_pool.hpp"

#include "dawn/native/DawnNative.h"
#include "dawn/dawn_proc.h"
#include "webgpu/webgpu_cpp.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

using namespace pulp::render::detail;

namespace {

// Create a standalone Dawn device for the pool to own buffers on.
// Returns {instance, device} or {nullptr, nullptr} if no GPU adapter
// is available (e.g., headless CI without a software Vulkan layer).
struct DawnEnv {
    std::unique_ptr<dawn::native::Instance> native_instance;
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;

    bool valid() const { return device != nullptr; }
};

DawnEnv make_dawn_env() {
    DawnEnv env;

    const DawnProcTable& procs = dawn::native::GetProcs();
    dawnProcSetProcs(&procs);

    wgpu::InstanceDescriptor inst_desc{};
    env.native_instance = std::make_unique<dawn::native::Instance>(
        reinterpret_cast<const WGPUInstanceDescriptor*>(&inst_desc));
    env.instance = wgpu::Instance(env.native_instance->Get());
    if (!env.instance) return env;

    wgpu::RequestAdapterOptions opts{};
    opts.powerPreference = wgpu::PowerPreference::HighPerformance;
    env.instance.RequestAdapter(
        &opts, wgpu::CallbackMode::AllowProcessEvents,
        [&env](wgpu::RequestAdapterStatus status, wgpu::Adapter result,
               wgpu::StringView) {
            if (status == wgpu::RequestAdapterStatus::Success)
                env.adapter = std::move(result);
        });
    env.instance.ProcessEvents();
    if (!env.adapter) return env;

    wgpu::DeviceDescriptor dev_desc{};
    dev_desc.label = "Pulp Pool Test Device";
    env.adapter.RequestDevice(
        &dev_desc, wgpu::CallbackMode::AllowProcessEvents,
        [&env](wgpu::RequestDeviceStatus status, wgpu::Device result,
               wgpu::StringView) {
            if (status == wgpu::RequestDeviceStatus::Success)
                env.device = std::move(result);
        });
    env.instance.ProcessEvents();

    return env;
}

}  // namespace

// ── next_pow2 — the math helper the pool uses for size rounding ─────

TEST_CASE("next_pow2 returns the next power of two", "[render][gpu][pool]") {
    REQUIRE(next_pow2(0) == 16);
    REQUIRE(next_pow2(1) == 16);
    REQUIRE(next_pow2(15) == 16);
    REQUIRE(next_pow2(16) == 16);
    REQUIRE(next_pow2(17) == 32);
    REQUIRE(next_pow2(100) == 128);
    REQUIRE(next_pow2(1024) == 1024);
    REQUIRE(next_pow2(1025) == 2048);
    REQUIRE(next_pow2(4096) == 4096);
    REQUIRE(next_pow2(4097) == 8192);
    REQUIRE(next_pow2(1u << 20) == (1u << 20));
}

// ── Pool hammer — repeated acquire/release bounds the allocation count ─

TEST_CASE("StagingBufferPool recycles buffers on rapid acquire/release",
          "[render][gpu][pool]") {
    auto env = make_dawn_env();
    if (!env.valid()) {
        WARN("no Dawn device available (headless CI without GPU); skipping");
        return;
    }

    std::size_t created = 0;
    StagingBufferPool::InstrumentationHooks hooks;
    hooks.created_count = &created;

    constexpr std::size_t kCap = 4;
    StagingBufferPool pool(env.device, kCap, hooks);

    const auto usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;

    // Acquire + immediately release 1000 times. After the first few
    // calls the pool should be warm; no more buffers should be created.
    for (int i = 0; i < 1000; ++i) {
        auto buf = pool.acquire(4096, usage);
        REQUIRE(buf != nullptr);
        pool.release(buf);
    }

    // Expectation: exactly one buffer instantiated for the steady-state
    // acquire/release pattern (we always release before the next acquire).
    // Allow a small cushion because Dawn may create internal helper
    // buffers, but the pool-tracked count should be 1.
    REQUIRE(created == 1);
    REQUIRE(pool.free_count() == 1);
    REQUIRE(pool.in_flight_count() == 0);
}

TEST_CASE("StagingBufferPool bounds allocation under mixed sizes",
          "[render][gpu][pool]") {
    auto env = make_dawn_env();
    if (!env.valid()) {
        WARN("no Dawn device available; skipping");
        return;
    }

    std::size_t created = 0;
    StagingBufferPool::InstrumentationHooks hooks;
    hooks.created_count = &created;

    constexpr std::size_t kCap = 8;
    StagingBufferPool pool(env.device, kCap, hooks);
    const auto usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;

    // Requests span 1 KB, 4 KB, 16 KB — three distinct power-of-two buckets.
    // Steady-state the pool should hold at most 3 distinct buffers, one per
    // bucket. Over 500 iterations the `created` counter must stay well
    // under the iteration count (proving we're not allocating every call).
    for (int i = 0; i < 500; ++i) {
        std::size_t bytes = 0;
        switch (i % 3) {
            case 0: bytes = 1024;  break;
            case 1: bytes = 4096;  break;
            case 2: bytes = 16384; break;
        }
        auto buf = pool.acquire(bytes, usage);
        REQUIRE(buf != nullptr);
        pool.release(buf);
    }

    // Three size buckets → at most 3 allocations for a fully-primed pool.
    // Allow a tiny amount of slack for scheduling interleave.
    REQUIRE(created <= 3);
    REQUIRE(pool.free_count() + pool.in_flight_count() <= kCap);
}

// ── Concurrent acquire/release — no TSan races ─────────────────────────

TEST_CASE("StagingBufferPool is safe under concurrent acquire/release",
          "[render][gpu][pool]") {
    auto env = make_dawn_env();
    if (!env.valid()) {
        WARN("no Dawn device available; skipping");
        return;
    }

    std::size_t created = 0;
    StagingBufferPool::InstrumentationHooks hooks;
    hooks.created_count = &created;

    constexpr std::size_t kCap = 16;
    StagingBufferPool pool(env.device, kCap, hooks);
    const auto usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;

    constexpr int kThreads = 4;
    constexpr int kCyclesPerThread = 100;
    std::atomic<int> successes{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&]() {
            for (int i = 0; i < kCyclesPerThread; ++i) {
                const std::size_t bytes = 1024 + (i % 8) * 512;
                auto buf = pool.acquire(bytes, usage);
                if (buf) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                    // Simulate a tiny amount of work between acquire and
                    // release so the threads actually interleave.
                    std::this_thread::yield();
                    pool.release(buf);
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    REQUIRE(successes.load() == kThreads * kCyclesPerThread);
    REQUIRE(pool.in_flight_count() == 0);
    // Total buffers owned by the pool must be bounded by cap.
    REQUIRE(pool.free_count() <= kCap);
}

// ── Destructor cleanup — no leaks on shutdown ──────────────────────────

TEST_CASE("StagingBufferPool destructor releases all buffers",
          "[render][gpu][pool]") {
    auto env = make_dawn_env();
    if (!env.valid()) {
        WARN("no Dawn device available; skipping");
        return;
    }

    std::size_t created = 0;
    StagingBufferPool::InstrumentationHooks hooks;
    hooks.created_count = &created;

    {
        StagingBufferPool pool(env.device, 4, hooks);
        const auto usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;

        // Populate free + in-flight maps both, to make sure the
        // destructor clears both paths.
        auto held = pool.acquire(4096, usage);
        {
            auto temp = pool.acquire(4096, usage);
            pool.release(temp);  // goes to free list
        }

        REQUIRE(pool.in_flight_count() == 1);
        REQUIRE(pool.free_count() == 1);
        // `held` falls out of scope, but the pool still holds its ref
        // via in_flight_. clear() in the destructor should drop those.
    }

    // If the destructor leaked, ASan would flag the buffers at process
    // exit. The fact that we reached here without aborting is the
    // success signal. We can't introspect the pool after destruction,
    // so we just assert the instrumentation counter's final value is
    // consistent.
    REQUIRE(created >= 1);
}

TEST_CASE("StagingBufferPool clear() drops all tracked buffers",
          "[render][gpu][pool]") {
    auto env = make_dawn_env();
    if (!env.valid()) {
        WARN("no Dawn device available; skipping");
        return;
    }

    StagingBufferPool pool(env.device, 4);
    const auto usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;

    auto a = pool.acquire(4096, usage);
    auto b = pool.acquire(4096, usage);
    pool.release(b);

    REQUIRE(pool.in_flight_count() == 1);
    REQUIRE(pool.free_count() == 1);

    pool.clear();

    REQUIRE(pool.in_flight_count() == 0);
    REQUIRE(pool.free_count() == 0);
}

TEST_CASE("StagingBufferPool::discard drops in_flight slot without recycling",
          "[render][gpu][pool][codex-560]") {
    // Codex 2026-04-21 wave 2 P1 on #560: on MapAsync timeout the
    // caller cannot hand the buffer back via release() (the GPU may
    // still be mapping it) but must drop the pool's reference so
    // subsequent acquires don't grow in_flight_ without bound. This
    // test hammers that path: 32 discarded buffers must leave both
    // free_count() and in_flight_count() bounded; without discard()
    // in_flight_count() would grow linearly.
    auto env = make_dawn_env();
    if (!env.valid()) {
        WARN("no Dawn device available; skipping");
        return;
    }

    std::size_t created = 0;
    StagingBufferPool::InstrumentationHooks hooks;
    hooks.created_count = &created;

    StagingBufferPool pool(env.device, 4, hooks);
    const auto usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;

    for (int i = 0; i < 32; ++i) {
        auto buf = pool.acquire(4096, usage);
        REQUIRE(buf != nullptr);
        // Simulate a timeout path: we can't release() because the GPU
        // may still hold a mapping. discard() drops the in_flight
        // reference so the pool reclaims its slot.
        pool.discard(buf);
    }

    // The pool cap is 4. Neither free_ nor in_flight_ should grow
    // beyond cap_ in the timeout-heavy path; if discard() were a
    // no-op, in_flight_count() would now be 32.
    REQUIRE(pool.in_flight_count() == 0);
    REQUIRE(pool.free_count() == 0);
}

TEST_CASE("StagingBufferPool separates pools by usage mask",
          "[render][gpu][pool]") {
    auto env = make_dawn_env();
    if (!env.valid()) {
        WARN("no Dawn device available; skipping");
        return;
    }

    std::size_t created = 0;
    StagingBufferPool::InstrumentationHooks hooks;
    hooks.created_count = &created;

    StagingBufferPool pool(env.device, 4, hooks);

    const auto storage_usage =
        wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
    const auto readback_usage =
        wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;

    auto s1 = pool.acquire(1024, storage_usage);
    pool.release(s1);

    // A readback-usage acquire should NOT reuse the storage-usage
    // buffer; Dawn forbids usage-mask overlap like that.
    auto r1 = pool.acquire(1024, readback_usage);
    REQUIRE(r1 != nullptr);
    REQUIRE(r1.Get() != s1.Get());
    pool.release(r1);

    REQUIRE(created == 2);
}

#else  // !PULP_HAS_SKIA

TEST_CASE("StagingBufferPool tests require Skia/Dawn", "[render][gpu][pool]") {
    SUCCEED("Skipped: PULP_HAS_SKIA not defined");
}

#endif  // PULP_HAS_SKIA
