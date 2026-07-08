// Shared click-free crossfade fixture for signal::TransitionMixer — the one
// primitive the hot-swap slot, convolver IR swapper, and SwapUnit transitions
// all fade through.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/transition_mixer.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

using pulp::signal::TransitionCurve;
using pulp::signal::TransitionMixer;
using Catch::Matchers::WithinAbs;

namespace {
// Sample new_gain across the whole fade; return the max consecutive delta.
double max_step(const TransitionMixer& m) {
    std::vector<float> ng;
    for (std::size_t p = 0; p <= m.length(); ++p) {
        float o, n;
        m.gains_at(p, o, n);
        ng.push_back(n);
    }
    double worst = 0.0;
    for (std::size_t i = 1; i < ng.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(ng[i] - ng[i - 1])));
    return worst;
}
}  // namespace

TEST_CASE("TransitionMixer endpoints and length semantics", "[signal][transition]") {
    TransitionMixer m;
    m.configure(256, TransitionCurve::Smoothstep);
    REQUIRE(m.length() == 256);
    REQUIRE(m.position() == 0);
    REQUIRE_FALSE(m.done());

    float o, n;
    m.gains_at(0, o, n);
    REQUIRE(o == Catch::Approx(1.0f));   // fully old at the start
    REQUIRE(n == Catch::Approx(0.0f));
    m.gains_at(256, o, n);
    REQUIRE(o == Catch::Approx(0.0f));   // fully new at the end
    REQUIRE(n == Catch::Approx(1.0f));
    m.gains_at(999, o, n);               // clamps past the end
    REQUIRE(n == Catch::Approx(1.0f));

    m.advance(256);
    REQUIRE(m.done());

    // length 0 == "no fade": immediately done, gains resolve to fully-new.
    TransitionMixer none;
    none.configure(0, TransitionCurve::Smoothstep);
    REQUIRE(none.done());
    none.gains_at(0, o, n);
    REQUIRE(n == Catch::Approx(1.0f));
}

TEST_CASE("TransitionMixer Smoothstep is equal-gain + click-free", "[signal][transition]") {
    TransitionMixer m;
    m.configure(512, TransitionCurve::Smoothstep);
    for (std::size_t p = 0; p <= 512; ++p) {
        float o, n;
        m.gains_at(p, o, n);
        REQUIRE_THAT(o + n, WithinAbs(1.0f, 1e-4f));   // equal-gain: sum == 1
        REQUIRE(o >= -1e-4f);
        REQUIRE(n >= -1e-4f);
    }
    // Click-free: no abrupt jump, and the midpoint is exactly 0.5 (smoothstep(0.5)).
    REQUIRE(max_step(m) < 0.02);
    float o, n; m.gains_at(256, o, n);
    REQUIRE(n == Catch::Approx(0.5f).margin(0.001));
}

TEST_CASE("TransitionMixer EqualPower is constant-power + click-free", "[signal][transition]") {
    TransitionMixer m;
    m.configure(512, TransitionCurve::EqualPower);
    for (std::size_t p = 0; p <= 512; ++p) {
        float o, n;
        m.gains_at(p, o, n);
        REQUIRE_THAT(o * o + n * n, WithinAbs(1.0f, 1e-4f));   // constant power: sum of squares == 1
    }
    // Click-free ends (cos/sin over the smoothstep ramp → zero slope at 0 and 1).
    REQUIRE(max_step(m) < 0.02);
    // Midpoint gains are cos/sin(pi/4) == 0.7071 (distinct from equal-gain's 0.5).
    float o, n; m.gains_at(256, o, n);
    REQUIRE(o == Catch::Approx(0.70710678f).margin(0.001));
    REQUIRE(n == Catch::Approx(0.70710678f).margin(0.001));
}

namespace {
// Minimal BufferView-shaped mock so we can exercise the templated blend_fade_out
// without pulling the audio buffer headers into this signal-only fixture.
struct MockView {
    std::vector<std::vector<float>> ch;
    std::size_t num_channels() const { return ch.size(); }
    std::size_t num_samples() const { return ch.empty() ? 0 : ch[0].size(); }
    std::span<float> channel(std::size_t i) { return std::span<float>(ch[i]); }
    std::span<const float> channel(std::size_t i) const { return std::span<const float>(ch[i]); }
};
}  // namespace

TEST_CASE("blend_fade_out is click-free and reports completion", "[transition][blend]") {
    const std::size_t frames = 64;

    SECTION("Smoothstep equal-gain: identical old/new stays flat at 1.0 (no click)") {
        TransitionMixer m;
        m.configure(frames, TransitionCurve::Smoothstep);
        MockView out{{std::vector<float>(frames, 1.0f)}};   // new render
        MockView old{{std::vector<float>(frames, 1.0f)}};   // fading-out render
        const bool done = pulp::signal::blend_fade_out(m, out, old);
        for (float v : out.ch[0]) CHECK_THAT(v, WithinAbs(1.0f, 1e-6));  // old_gain+new_gain==1
        CHECK(done);  // advanced by frames == configured length
    }
    SECTION("EqualPower: identical old/new stays bounded in [1, sqrt2]") {
        TransitionMixer m;
        m.configure(frames, TransitionCurve::EqualPower);
        MockView out{{std::vector<float>(frames, 1.0f)}};
        MockView old{{std::vector<float>(frames, 1.0f)}};
        pulp::signal::blend_fade_out(m, out, old);
        for (float v : out.ch[0]) {
            CHECK(v >= 0.99f);
            CHECK(v <= 1.4143f);  // cos+sin peaks at sqrt2
        }
    }
    SECTION("a completed mixer reports done and leaves output as the new render") {
        TransitionMixer m;
        m.configure(frames, TransitionCurve::Smoothstep);
        MockView out{{std::vector<float>(frames, 0.5f)}};
        MockView old{{std::vector<float>(frames, 0.0f)}};
        CHECK(pulp::signal::blend_fade_out(m, out, old));  // one block finishes it
        // second call: mixer already done -> gains are full-new, output unchanged
        MockView out2{{std::vector<float>(frames, 0.5f)}};
        MockView old2{{std::vector<float>(frames, 9.0f)}};
        CHECK(pulp::signal::blend_fade_out(m, out2, old2));
        for (float v : out2.ch[0]) CHECK_THAT(v, WithinAbs(0.5f, 1e-6));  // old fully faded out
    }
}
