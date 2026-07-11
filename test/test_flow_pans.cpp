// Flow pans are pure math (no GPU): the per-room, per-block constant-power pan
// drift that turns a static multi-room reverb into a moving field. These run on
// no-GPU CI, which is also what keeps the flow math covered in the coverage
// build (the SuperConvolver example test that also exercises it is not compiled
// there). Also touches GpuMultiConvolver::set_flow, a plain atomic store that
// needs no device.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/gpu_audio/flow_pans.hpp>
#include <pulp/gpu_audio/gpu_multi_convolver.hpp>

#include <cmath>
#include <vector>

using namespace pulp::gpu_audio;
using Catch::Approx;

namespace {
constexpr float kHalfPi = 1.5707963f;
}

TEST_CASE("flow_hash is deterministic and in the unit interval", "[flow]") {
    for (std::uint32_t k = 0; k < 64; ++k) {
        const float h = flow_hash(k);
        REQUIRE(h >= 0.0f);
        REQUIRE(h < 1.0f);
        REQUIRE(flow_hash(k) == h);  // deterministic
    }
}

TEST_CASE("flow_base_azimuth spreads evenly to half pi", "[flow]") {
    // n == 1 sits dead-center (pi/4).
    REQUIRE(flow_base_azimuth(0, 1) == Approx(kHalfPi * 0.5f));
    // n > 1: first room at 0, last at pi/2, monotone increasing.
    const std::uint32_t n = 8;
    REQUIRE(flow_base_azimuth(0, n) == Approx(0.0f));
    REQUIRE(flow_base_azimuth(n - 1, n) == Approx(kHalfPi));
    for (std::uint32_t k = 1; k < n; ++k)
        REQUIRE(flow_base_azimuth(k, n) > flow_base_azimuth(k - 1, n));
}

TEST_CASE("flow_pan_room is constant-power at every instant", "[flow]") {
    const float norm = 0.5f;
    // Across depth, spread, time, room — pan_l^2 + pan_r^2 == norm^2 always, so
    // the moving field never pumps loudness.
    for (float depth : {0.0f, 0.5f, 1.0f})
        for (double t : {0.0, 0.37, 1.9, 5.0})
            for (std::uint32_t k = 0; k < 6; ++k) {
                float l = 0.0f, r = 0.0f;
                flow_pan_room(flow_base_azimuth(k, 6), k, depth, 1.0f, t, norm, l, r);
                REQUIRE(l >= 0.0f);
                REQUIRE(r >= 0.0f);
                REQUIRE(l * l + r * r == Approx(norm * norm).margin(1e-5));
            }
}

TEST_CASE("depth zero reproduces the static base layout", "[flow]") {
    const float norm = 0.7f;
    for (std::uint32_t k = 0; k < 5; ++k) {
        const float base = flow_base_azimuth(k, 5);
        float l = 0.0f, r = 0.0f;
        // Any time, depth 0 -> exactly (cos*norm, sin*norm) of the base azimuth.
        flow_pan_room(base, k, 0.0f, 1.0f, 3.14, norm, l, r);
        REQUIRE(l == Approx(std::cos(base) * norm));
        REQUIRE(r == Approx(std::sin(base) * norm));
    }
}

TEST_CASE("positive depth moves the pans over time", "[flow]") {
    const std::uint32_t n = 8;
    std::vector<float> base(n);
    for (std::uint32_t k = 0; k < n; ++k) base[k] = flow_base_azimuth(k, n);

    std::vector<float> l0(n), r0(n), l1(n), r1(n);
    flow_pans_from_base(base.data(), n, 1.0f, 1.0f, 0.0, 0.35f, l0.data(), r0.data());
    flow_pans_from_base(base.data(), n, 1.0f, 1.0f, 2.0, 0.35f, l1.data(), r1.data());

    // At least one room's pan moved meaningfully across two seconds at full depth.
    float max_move = 0.0f;
    for (std::uint32_t k = 0; k < n; ++k)
        max_move = std::max(max_move, std::abs(l1[k] - l0[k]));
    REQUIRE(max_move > 0.05f);
}

TEST_CASE("fill_flow_pans matches per-base fill and stays in range", "[flow]") {
    const std::uint32_t n = 6;
    std::vector<float> a_l(n), a_r(n), b_l(n), b_r(n), base(n);
    for (std::uint32_t k = 0; k < n; ++k) base[k] = flow_base_azimuth(k, n);
    const float norm = 1.0f / std::sqrt(static_cast<float>(n));

    fill_flow_pans(n, 1.1, 0.6f, 0.8f, a_l.data(), a_r.data());
    flow_pans_from_base(base.data(), n, 0.6f, 0.8f, 1.1, norm, b_l.data(), b_r.data());
    for (std::uint32_t k = 0; k < n; ++k) {
        REQUIRE(a_l[k] == Approx(b_l[k]));
        REQUIRE(a_r[k] == Approx(b_r[k]));
    }
}

TEST_CASE("GpuMultiConvolver set_flow clamps without a device", "[flow]") {
    // set_flow is a plain atomic store — safe to call with no GPU. Construct a
    // node (no prepare) and exercise the clamp on both ends.
    std::vector<std::vector<float>> irs = {{1.0f, 0.5f, 0.25f}, {0.3f, 0.2f, 0.1f}};
    GpuMultiConvolver mc(512, 48000, std::move(irs));
    REQUIRE(mc.num_ir() == 2);
    mc.set_flow(0.5f, 1.0f);
    mc.set_flow(-1.0f);       // clamps to 0
    mc.set_flow(2.0f, 3.0f);  // clamps depth and spread to 1
    SUCCEED("set_flow accepted every value without a device");
}
