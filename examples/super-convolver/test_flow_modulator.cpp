// Tests for the Flow pan modulator: static at flow=0, moving + full-rank above,
// constant-power at every instant.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/gpu_audio/flow_pans.hpp>

#include <cmath>
#include <vector>

using namespace pulp::gpu_audio;
using Catch::Matchers::WithinAbs;

namespace {
// The constant-power layout the GPU engine bakes today (Flow=0 must match it).
void static_pans(std::uint32_t n, std::vector<float>& l, std::vector<float>& r) {
    l.assign(n, 0.f); r.assign(n, 0.f);
    const float norm = 1.f / std::sqrt(static_cast<float>(n));
    for (std::uint32_t k = 0; k < n; ++k) {
        const float t = (n == 1) ? 0.7853982f
                                 : static_cast<float>(k) / (n - 1) * 1.5707963f;
        l[k] = std::cos(t) * norm; r[k] = std::sin(t) * norm;
    }
}
}  // namespace

TEST_CASE("Flow=0 reproduces the static constant-power layout", "[superconvolver][flow]") {
    for (std::uint32_t n : {1u, 2u, 8u, 32u}) {
        std::vector<float> pl(n), pr(n), sl, sr;
        static_pans(n, sl, sr);
        // Any time value — at flow=0 the result is time-independent.
        fill_flow_pans(n, 12.34, /*flow=*/0.f, /*spread=*/1.f, pl.data(), pr.data());
        for (std::uint32_t k = 0; k < n; ++k) {
            REQUIRE_THAT(pl[k], WithinAbs(sl[k], 1e-6));
            REQUIRE_THAT(pr[k], WithinAbs(sr[k], 1e-6));
        }
    }
}

TEST_CASE("Pans stay constant-power at every flow and instant", "[superconvolver][flow]") {
    const std::uint32_t n = 24;
    std::vector<float> pl(n), pr(n);
    const float inv_n = 1.f / static_cast<float>(n);
    for (float flow : {0.f, 0.4f, 1.f})
        for (double t : {0.0, 0.37, 5.1, 20.0}) {
            fill_flow_pans(n, t, flow, 0.8f, pl.data(), pr.data());
            for (std::uint32_t k = 0; k < n; ++k) {
                REQUIRE_THAT(pl[k] * pl[k] + pr[k] * pr[k], WithinAbs(inv_n, 1e-5));
                REQUIRE(pl[k] >= -1e-6f);   // in range [0, norm]
                REQUIRE(pr[k] >= -1e-6f);
            }
        }
}

TEST_CASE("Flow>0 moves the field over time", "[superconvolver][flow]") {
    const std::uint32_t n = 16;
    std::vector<float> a_l(n), a_r(n), b_l(n), b_r(n);
    fill_flow_pans(n, 0.0, 0.8f, 1.f, a_l.data(), a_r.data());
    fill_flow_pans(n, 1.0, 0.8f, 1.f, b_l.data(), b_r.data());
    float max_move = 0.f;
    for (std::uint32_t k = 0; k < n; ++k)
        max_move = std::max(max_move, std::fabs(a_l[k] - b_l[k]));
    REQUIRE(max_move > 0.02f);  // the field visibly moved
}

TEST_CASE("Rooms move independently (full-rank, not lockstep)", "[superconvolver][flow]") {
    const std::uint32_t n = 32;
    std::vector<float> t0l(n), t0r(n), t1l(n), t1r(n);
    fill_flow_pans(n, 0.0, 1.f, 1.f, t0l.data(), t0r.data());
    fill_flow_pans(n, 0.5, 1.f, 1.f, t1l.data(), t1r.data());
    // Per-room delta over the same interval must vary across rooms — if every
    // room shared one motion the deltas would be (nearly) identical.
    std::vector<float> delta(n);
    for (std::uint32_t k = 0; k < n; ++k) delta[k] = t1l[k] - t0l[k];
    float lo = delta[0], hi = delta[0];
    for (float d : delta) { lo = std::min(lo, d); hi = std::max(hi, d); }
    REQUIRE((hi - lo) > 0.05f);  // rooms genuinely decorrelated
}
