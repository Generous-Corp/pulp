// What a window host records about a swapchain acquire (gpu_acquire span args).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/view/gpu_acquire_diagnostics.hpp>

#include <limits>

using pulp::view::gpu_acquire_diagnostics;
using pulp::view::gpu_timing_requested;

TEST_CASE("gpu_acquire_diagnostics measures lateness against the display-link target",
          "[view][gpu-acquire]") {
    // 120 Hz display, acquiring 3 ms after the target presentation time.
    const auto late = gpu_acquire_diagnostics(10.003, 10.0, 1.0 / 120.0, 2);
    CHECK(late.vsync_driven);
    CHECK(late.late_ms == Catch::Approx(3.0).margin(1e-6));
    CHECK(late.refresh_period_ms == Catch::Approx(8.3333).margin(1e-3));
    CHECK(late.frames_in_flight == 2);

    // Rendering ahead of the target reads as negative lateness.
    const auto early = gpu_acquire_diagnostics(9.99, 10.0, 1.0 / 60.0, 0);
    CHECK(early.late_ms == Catch::Approx(-10.0).margin(1e-6));
}

TEST_CASE("gpu_acquire_diagnostics marks frames rendered outside the display link",
          "[view][gpu-acquire]") {
    const auto d = gpu_acquire_diagnostics(10.0, 0.0, 0.0, -1);
    CHECK_FALSE(d.vsync_driven);
    CHECK(d.late_ms == 0.0);
    CHECK(d.refresh_period_ms == 0.0);
    CHECK(d.frames_in_flight == -1);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto bad = gpu_acquire_diagnostics(10.0, nan, nan, 1);
    CHECK_FALSE(bad.vsync_driven);
    CHECK(bad.refresh_period_ms == 0.0);
}

TEST_CASE("gpu_timing_requested accepts only explicit truthy values",
          "[view][gpu-acquire]") {
    CHECK(gpu_timing_requested("1"));
    CHECK(gpu_timing_requested("true"));
    CHECK(gpu_timing_requested("YES"));
    CHECK(gpu_timing_requested("On"));
    CHECK_FALSE(gpu_timing_requested(nullptr));
    CHECK_FALSE(gpu_timing_requested(""));
    CHECK_FALSE(gpu_timing_requested("0"));
    CHECK_FALSE(gpu_timing_requested("false"));
    CHECK_FALSE(gpu_timing_requested("enabled"));
}
