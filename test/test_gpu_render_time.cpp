// Phase 6.5 (re-scoped) — GPU render time pure-logic tests.
//
// The Dawn/Graphite plumbing (SkiaSurface's GpuStats callback) needs a live
// GPU device + window and is exercised by the on-device smoke / CI macOS lane.
// The pure seam — ns→ms conversion + the "0 or failed callback == no sample"
// rule + the cross-thread latest-sample holder — is Dawn-free and tested here
// with synthetic samples, the same way DwmBackendTracker / the old per-pass
// timestamp math are unit-tested without a GPU.

#include <catch2/catch_test_macros.hpp>

#include <pulp/render/gpu_render_time.hpp>

using namespace pulp::render;

TEST_CASE("gpu_render_ns_to_ms converts a successful sample",
          "[render][gpu-render-time][issue-2611]") {
    // 1 ms == 1e6 ns.
    auto ms = gpu_render_ns_to_ms(1'000'000, /*callback_ok=*/true);
    REQUIRE(ms.has_value());
    REQUIRE(*ms == 1.0);

    // 16.667 ms ≈ a 60 Hz frame.
    auto frame = gpu_render_ns_to_ms(16'667'000, true);
    REQUIRE(frame.has_value());
    REQUIRE(*frame > 16.6);
    REQUIRE(*frame < 16.7);
}

TEST_CASE("gpu_render_ns_to_ms reports no sample for zero or failed callbacks",
          "[render][gpu-render-time][issue-2611]") {
    // Zero elapsed is Skia's "no pass timestamped / timer failed" sentinel.
    REQUIRE_FALSE(gpu_render_ns_to_ms(0, true).has_value());
    // A failed callback is never a usable duration, even with non-zero ns.
    REQUIRE_FALSE(gpu_render_ns_to_ms(5'000'000, false).has_value());
    REQUIRE_FALSE(gpu_render_ns_to_ms(0, false).has_value());
}

TEST_CASE("GpuRenderTimeTracker starts with no sample",
          "[render][gpu-render-time][issue-2611]") {
    GpuRenderTimeTracker t;
    REQUIRE_FALSE(t.have_sample());
    REQUIRE(t.last_ms() == 0.0);
}

TEST_CASE("GpuRenderTimeTracker stores a valid sample",
          "[render][gpu-render-time][issue-2611]") {
    GpuRenderTimeTracker t;
    t.store(2'000'000, /*callback_ok=*/true);  // 2 ms
    REQUIRE(t.have_sample());
    REQUIRE(t.last_ms() == 2.0);
}

TEST_CASE("GpuRenderTimeTracker retains the last good sample across no-sample frames",
          "[render][gpu-render-time][issue-2611]") {
    GpuRenderTimeTracker t;
    t.store(3'000'000, true);            // 3 ms — good
    REQUIRE(t.last_ms() == 3.0);

    // A failed callback or zero-elapsed frame must NOT clobber the last good
    // value to 0 — the inspector keeps showing the most recent real duration.
    t.store(0, true);
    REQUIRE(t.have_sample());
    REQUIRE(t.last_ms() == 3.0);

    t.store(9'000'000, false);
    REQUIRE(t.last_ms() == 3.0);

    // A new valid sample updates it.
    t.store(5'000'000, true);            // 5 ms
    REQUIRE(t.last_ms() == 5.0);
}
