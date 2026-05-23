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
#include <pulp/render/render_pass.hpp>

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

// ── RenderPassManager frame-level (whole-recording) GPU render time ──────────
// Follow-up #2611 plumbing: a FRAME-level GPU render time on the manager,
// distinct from the per-pass set_pass_gpu_time(). This is the value the
// WindowHost forwards from SkiaSurface::gpu_render_time_ms() and that the
// inspector surfaces as `gpu_render_time_ms`.

TEST_CASE("RenderPassManager frame GPU render time starts unavailable",
          "[render][gpu-render-time][render-pass][issue-2611]") {
    RenderPassManager rpm;
    REQUIRE_FALSE(rpm.gpu_render_timing_available());
    REQUIRE(rpm.gpu_render_time_ms() == 0.0f);
}

TEST_CASE("RenderPassManager stores a valid frame GPU render time",
          "[render][gpu-render-time][render-pass][issue-2611]") {
    RenderPassManager rpm;
    rpm.set_gpu_render_time_ms(4.5f, /*valid=*/true);
    REQUIRE(rpm.gpu_render_timing_available());
    REQUIRE(rpm.gpu_render_time_ms() == 4.5f);
}

TEST_CASE("RenderPassManager rejects an invalid or negative frame GPU sample",
          "[render][gpu-render-time][render-pass][issue-2611]") {
    RenderPassManager rpm;

    // valid=false never marks the sample available, regardless of ms.
    rpm.set_gpu_render_time_ms(7.0f, /*valid=*/false);
    REQUIRE_FALSE(rpm.gpu_render_timing_available());

    // A negative duration is treated as "no sample" even when valid=true.
    rpm.set_gpu_render_time_ms(-1.0f, /*valid=*/true);
    REQUIRE_FALSE(rpm.gpu_render_timing_available());
}

TEST_CASE("RenderPassManager frame GPU validity resets on begin_frame",
          "[render][gpu-render-time][render-pass][issue-2611]") {
    RenderPassManager rpm;
    rpm.set_gpu_render_time_ms(3.0f, /*valid=*/true);
    REQUIRE(rpm.gpu_render_timing_available());
    REQUIRE(rpm.gpu_render_time_ms() == 3.0f);

    // A fresh frame invalidates last frame's sample so a stale read between
    // begin_frame() and the next set is reported as unavailable...
    rpm.begin_frame();
    REQUIRE_FALSE(rpm.gpu_render_timing_available());
    // ...but the prior value is retained until a new sample lands, so the
    // inspector can keep showing the most-recent real number with a stale flag.
    REQUIRE(rpm.gpu_render_time_ms() == 3.0f);

    // Feeding this frame's sample restores availability.
    rpm.set_gpu_render_time_ms(6.0f, /*valid=*/true);
    REQUIRE(rpm.gpu_render_timing_available());
    REQUIRE(rpm.gpu_render_time_ms() == 6.0f);
}

TEST_CASE("RenderPassManager frame GPU time is independent of per-pass GPU time",
          "[render][gpu-render-time][render-pass][issue-2611]") {
    RenderPassManager rpm;
    rpm.begin_frame();
    rpm.begin_pass(RenderPassType::content);
    rpm.end_pass(5.0f, 10);
    rpm.end_frame();

    // Per-pass GPU timing and frame-level GPU render time are separate seams.
    REQUIRE_FALSE(rpm.has_gpu_timing());
    REQUIRE_FALSE(rpm.gpu_render_timing_available());

    // Set only the per-pass clock: frame-level stays unavailable.
    rpm.set_pass_gpu_time(0, 2.0f);
    REQUIRE(rpm.has_gpu_timing());
    REQUIRE_FALSE(rpm.gpu_render_timing_available());

    // Set only the frame-level clock on a fresh frame: per-pass stays empty.
    rpm.begin_frame();
    rpm.set_gpu_render_time_ms(8.0f, true);
    REQUIRE(rpm.gpu_render_timing_available());
    REQUIRE(rpm.gpu_render_time_ms() == 8.0f);
    REQUIRE_FALSE(rpm.has_gpu_timing());
}

TEST_CASE("RenderPassManager handles empty pass endings and disabled budgets",
          "[render][gpu-render-time][render-pass][coverage][phase3-large]") {
    RenderPassManager rpm;
    REQUIRE(rpm.budget() == 16.67f);
    REQUIRE(rpm.frame_count() == 0);
    REQUIRE(rpm.current_pass() == RenderPassType::background);

    rpm.end_pass(12.0f, 4);
    rpm.end_frame();
    REQUIRE(rpm.passes().empty());
    REQUIRE(rpm.total_time_ms() == 0.0f);
    REQUIRE_FALSE(rpm.over_budget());

    rpm.set_budget(0.0f);
    REQUIRE(rpm.budget() == 0.0f);
    rpm.begin_frame();
    rpm.begin_pass(RenderPassType::post_effects);
    rpm.end_pass(100.0f, 7);
    rpm.end_frame();

    REQUIRE(rpm.frame_count() == 1);
    REQUIRE(rpm.current_pass() == RenderPassType::post_effects);
    REQUIRE(rpm.passes().size() == 1);
    REQUIRE(rpm.passes().front().type == RenderPassType::post_effects);
    REQUIRE(rpm.passes().front().draw_calls == 7);
    REQUIRE(rpm.passes().front().cpu_time_ms() == 100.0f);
    REQUIRE(rpm.total_time_ms() == 100.0f);
    REQUIRE_FALSE(rpm.over_budget());
}
