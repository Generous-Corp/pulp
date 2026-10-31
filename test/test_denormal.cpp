#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/svf.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace pulp::signal;

TEST_CASE("snap_to_zero passes through audible values", "[signal][denormal]") {
    REQUIRE(snap_to_zero(1.0f) == 1.0f);
    REQUIRE(snap_to_zero(-1.0f) == -1.0f);
    REQUIRE(snap_to_zero(1e-10f) == 1e-10f);  // -200 dB FS, still passes
    REQUIRE(snap_to_zero(-1e-10f) == -1e-10f);
}

TEST_CASE("snap_to_zero zeroes denormals and tiny values", "[signal][denormal]") {
    // Below 1e-15f threshold — should snap to zero.
    REQUIRE(snap_to_zero(1e-20f) == 0.0f);
    REQUIRE(snap_to_zero(-1e-20f) == 0.0f);
    REQUIRE(snap_to_zero(std::numeric_limits<float>::denorm_min()) == 0.0f);
    REQUIRE(snap_to_zero(-std::numeric_limits<float>::denorm_min()) == 0.0f);
}

TEST_CASE("snap_to_zero is identity at exact zero", "[signal][denormal]") {
    REQUIRE(snap_to_zero(0.0f) == 0.0f);
    REQUIRE(snap_to_zero(-0.0f) == 0.0f);
}

TEST_CASE("snap_to_zero buffer overload", "[signal][denormal]") {
    float buf[8] = {1.0f, -1.0f, 1e-20f, -1e-20f, 0.0f, 0.5f, -0.5f,
                    std::numeric_limits<float>::denorm_min()};
    snap_to_zero(buf, 8);
    REQUIRE(buf[0] == 1.0f);
    REQUIRE(buf[1] == -1.0f);
    REQUIRE(buf[2] == 0.0f);
    REQUIRE(buf[3] == 0.0f);
    REQUIRE(buf[4] == 0.0f);
    REQUIRE(buf[5] == 0.5f);
    REQUIRE(buf[6] == -0.5f);
    REQUIRE(buf[7] == 0.0f);
}

TEST_CASE("is_denormal classifies subnormals", "[signal][denormal]") {
    REQUIRE_FALSE(is_denormal(0.0f));
    REQUIRE_FALSE(is_denormal(1.0f));
    REQUIRE_FALSE(is_denormal(1e-10f));   // tiny but normal
    REQUIRE(is_denormal(std::numeric_limits<float>::denorm_min()));
}

TEST_CASE("snap_to_zero double precision", "[signal][denormal]") {
    REQUIRE(snap_to_zero(1.0) == 1.0);
    REQUIRE(snap_to_zero(1e-20) == 0.0);
    REQUIRE(snap_to_zero(0.5) == 0.5);
}

// ---- Per-sample recursion state: snapped only where subnormals are slow ----

namespace {

// Feeds a unit impulse then silence and returns the output sequence, long
// enough for a lowpass tail to decay through 1e-15 and well past it.
template <typename Fn> std::vector<float> impulse_tail(Fn&& process, std::size_t length) {
    std::vector<float> out(length);
    for (std::size_t i = 0; i < length; ++i)
        out[i] = process(i == 0 ? 1.0f : 0.0f);
    return out;
}

// When the state is snapped every sample, a decaying tail jumps from about
// the snap threshold (1e-15) straight to zero, so no output lands far below
// it. Where subnormals are free the state keeps decaying, and the tail must
// pass through 1e-20 on its way to underflow.
void require_tail_matches_target(const std::vector<float>& tail) {
    constexpr float kFarBelowThreshold = 1e-20f;
    const bool deep_tail = std::any_of(tail.begin(), tail.end(), [](float v) {
        return v != 0.0f && std::abs(v) < kFarBelowThreshold;
    });
    if constexpr (kSubnormalsAreFree)
        REQUIRE(deep_tail);
    else
        REQUIRE_FALSE(deep_tail);
    REQUIRE(std::all_of(tail.begin(), tail.end(), [](float v) { return std::isfinite(v); }));
}

} // namespace

TEST_CASE("kSubnormalsAreFree names 64-bit ARM targets", "[signal][denormal]") {
#if defined(__aarch64__) || defined(_M_ARM64)
    STATIC_REQUIRE(kSubnormalsAreFree);
#else
    STATIC_REQUIRE_FALSE(kSubnormalsAreFree);
#endif
}

TEST_CASE("snap_recursion_state snaps only where subnormals are slow", "[signal][denormal]") {
    for (const float v :
         {1.0f, -0.5f, 1e-10f, 1e-20f, -1e-20f, std::numeric_limits<float>::denorm_min(), 0.0f}) {
        const float expected = kSubnormalsAreFree ? v : snap_to_zero(v);
        REQUIRE(snap_recursion_state(v) == expected);
    }
    REQUIRE(snap_recursion_state(1e-300) == (kSubnormalsAreFree ? 1e-300 : 0.0));
}

TEST_CASE("Biquad per-sample path matches a snapping DF2T until the state first snaps",
          "[signal][denormal][biquad]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 400.0f, 0.7071f, 48000.0f);
    const auto c = filter.coefficients();

    // The per-sample path as it was before snapping became target-dependent,
    // recording the first sample at which a snap changes a state value.
    float s1 = 0.0f, s2 = 0.0f;
    std::size_t first_snap = 0;
    std::size_t n = 0;
    auto snapping = [&](float x) {
        const float y = c.b0 * x + s1;
        const float raw1 = c.b1 * x - c.a1 * y + s2;
        const float raw2 = c.b2 * x - c.a2 * y;
        s1 = snap_to_zero(raw1);
        s2 = snap_to_zero(raw2);
        if (first_snap == 0 && (s1 != raw1 || s2 != raw2))
            first_snap = n + 1; // the next output is the first a snap can reach
        ++n;
        return y;
    };
    const auto reference = impulse_tail(snapping, 48000);
    const auto actual = impulse_tail([&](float x) { return filter.process(x); }, 48000);
    REQUIRE(first_snap > 0);

    // Bit-identical until the reference first snaps a state. After that the
    // reference's zeroed component still feeds its tail, so the two differ at
    // the tail's own scale: at most 7.0e-14 (about -263 dB) across 100 Hz to
    // 8 kHz lowpasses; the bound below leaves margin for other targets.
    constexpr float kTailBound = 1e-12f;
    for (std::size_t i = 0; i < first_snap; ++i) {
        INFO("sample " << i);
        REQUIRE(actual[i] == reference[i]);
    }
    for (std::size_t i = first_snap; i < reference.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(std::abs(actual[i] - reference[i]) <= kTailBound);
    }
    if constexpr (!kSubnormalsAreFree)
        REQUIRE(actual == reference);
    require_tail_matches_target(actual);
}

TEST_CASE("TptFilter and Svf per-sample tails snap only where subnormals are slow",
          "[signal][denormal]") {
    TptFilter tpt;
    tpt.prepare(48000.0f);
    tpt.set_cutoff(200.0f);
    require_tail_matches_target(
        impulse_tail([&](float x) { return tpt.process_lowpass(x); }, 48000));

    TptFilter tpt_all;
    tpt_all.prepare(48000.0f);
    tpt_all.set_cutoff(200.0f);
    require_tail_matches_target(
        impulse_tail([&](float x) { return tpt_all.process(x).lowpass; }, 48000));

    Svf svf;
    svf.configure(Svf::Mode::lowpass, 48000.0f, 200.0f, 0.7071f);
    require_tail_matches_target(impulse_tail([&](float x) { return svf.process(x); }, 48000));
}
