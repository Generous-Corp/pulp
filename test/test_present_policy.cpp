// test_present_policy.cpp — presentation and GPU-diagnostics policy (WAH-13).
//
// Both settings were previously implicit and inconsistent:
//
//   * The Windows editor host hardcoded `cfg.vsync = false` with the rationale
//     in a comment; the Linux host set nothing at all and silently inherited
//     GpuSurface's `vsync = true` default — the blocking Fifo mode the Windows
//     Perfetto measurement showed is wrong for an embedded editor. Two hosts,
//     two different unexplained answers.
//   * Dawn timestamp queries were requested whenever the ADAPTER advertised
//     them. Requesting them forces the `allow_unsafe_apis` toggle, so ordinary
//     rendering silently ran with relaxed validation on most machines — a
//     device-posture change nobody asked for, as a side effect of a diagnostic
//     nobody enabled.
//
// These are cheap value-level assertions on purpose: the point is that the
// policy is now a DECLARED default in one place that both hosts read, so a
// regression is a changed constant rather than a re-measured drag in a DAW.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/plugin_view_host.hpp>

// The GpuSurface half is header-only here (a default-constructed Config), but
// the header is not on the include path in a no-GPU build — which is exactly
// the configuration the diff-coverage lane uses. Guard it rather than linking
// pulp::render, which does not exist there at all.
#if __has_include(<pulp/render/gpu_surface.hpp>)
#  include <pulp/render/gpu_surface.hpp>
#  define PULP_TEST_HAS_GPU_SURFACE_CONFIG 1
#endif

using pulp::view::PluginViewHost;

TEST_CASE("an embedded editor host defaults to non-blocking present",
          "[present-policy][wah-13]") {
    // The measured default. An embedded editor renders synchronously from the
    // DAW's paint message on the DAW's UI thread; blocking on acquire stalls
    // the message pump that delivers further input, which is why only 7 frames
    // were produced across 8 drag sweeps before this was fixed.
    PluginViewHost::Options opts;
    REQUIRE(opts.present_policy == PluginViewHost::PresentPolicy::nonblocking);
}

TEST_CASE("GPU timing diagnostics are OFF by default",
          "[present-policy][wah-13]") {
    // Load-bearing: enabling timing forces Dawn's allow_unsafe_apis toggle,
    // which weakens validation for ordinary rendering too. A diagnostic must
    // not change the device's security posture as a side effect.
    PluginViewHost::Options opts;
    REQUIRE_FALSE(opts.enable_gpu_timing);
}

#ifdef PULP_TEST_HAS_GPU_SURFACE_CONFIG
TEST_CASE("GpuSurface does not request timestamps unless asked",
          "[present-policy][wah-13]") {
    pulp::render::GpuSurface::Config cfg;
    REQUIRE_FALSE(cfg.enable_gpu_timing);
}

TEST_CASE("GpuSurface still defaults to vsync for frame-loop-owning hosts",
          "[present-policy][wah-13]") {
    // Unchanged on purpose. A standalone window host owns its frame loop and
    // pacing to the refresh is the point; only the EMBEDDED policy differs.
    pulp::render::GpuSurface::Config cfg;
    REQUIRE(cfg.vsync);
}
#endif

TEST_CASE("the two present policies map to opposite vsync requests",
          "[present-policy][wah-13]") {
    // Pins the translation both hosts perform, so Windows and Linux cannot
    // drift back into two different answers.
    const auto to_vsync = [](PluginViewHost::PresentPolicy p) {
        return p == PluginViewHost::PresentPolicy::vsync;
    };
    REQUIRE(to_vsync(PluginViewHost::PresentPolicy::vsync));
    REQUIRE_FALSE(to_vsync(PluginViewHost::PresentPolicy::nonblocking));
}

// ── The policy is now translated in ONE place (WAH-6/WAH-13) ────────────────
//
// The assertions above pin the DEFAULT. These pin the TRANSLATION: both editor
// hosts build their surface through `create_editor_surfaces()`, which derives
// the config from `editor_surface_config()`. Before that existed, each host
// wrote its own `cfg.vsync = ...` line and they disagreed for months without
// anything able to observe it — the Linux host had no line at all and silently
// took the blocking default.
//
// `editor_surface_config()` is pure precisely so this runs on the required
// macOS gate with no GPU, no window, and no DAW.

#if __has_include(<pulp/view/plugin_frame_renderer.hpp>) && \
    defined(PULP_TEST_HAS_GPU_SURFACE_CONFIG)
#  include <pulp/view/plugin_frame_renderer.hpp>

TEST_CASE("the embedded default translates to a non-blocking present",
          "[present-policy][wah-13]") {
    pulp::view::FrameGeometry g;
    g.width = 400.0f;
    g.height = 300.0f;
    g.scale = 2.0f;

    const pulp::view::PluginViewHost::Options defaults;
    const auto cfg = pulp::view::editor_surface_config(
        nullptr, g, defaults.present_policy, defaults.enable_gpu_timing);

    // The regression this stops: a host reverting to Fifo and blocking the
    // DAW's UI thread on acquire.
    REQUIRE_FALSE(cfg.vsync);
    REQUIRE_FALSE(cfg.enable_gpu_timing);
}

TEST_CASE("an explicit vsync policy is still honoured",
          "[present-policy][wah-13]") {
    // A host that OWNS its frame loop must still be able to ask for Fifo; the
    // fix was to make the choice explicit, not to remove it.
    pulp::view::FrameGeometry g;
    g.width = 100.0f;
    g.height = 100.0f;

    const auto cfg = pulp::view::editor_surface_config(
        nullptr, g, pulp::view::PluginViewHost::PresentPolicy::vsync, false);

    REQUIRE(cfg.vsync);
}

TEST_CASE("the surface config is sized in physical pixels",
          "[present-policy][wah-13]") {
    // Same arithmetic the raster fallback and the headless capture use, so a
    // capture cannot disagree with what was presented.
    pulp::view::FrameGeometry g;
    g.width = 400.0f;
    g.height = 300.0f;
    g.scale = 2.0f;

    const auto cfg = pulp::view::editor_surface_config(
        nullptr, g, pulp::view::PluginViewHost::PresentPolicy::nonblocking, false);

    REQUIRE(cfg.width == 800);
    REQUIRE(cfg.height == 600);
}

TEST_CASE("gpu timing is opt-in, not adapter-driven",
          "[present-policy][wah-13]") {
    pulp::view::FrameGeometry g;
    g.width = 10.0f;
    g.height = 10.0f;

    const auto on = pulp::view::editor_surface_config(
        nullptr, g, pulp::view::PluginViewHost::PresentPolicy::nonblocking, true);
    REQUIRE(on.enable_gpu_timing);
}
#endif
