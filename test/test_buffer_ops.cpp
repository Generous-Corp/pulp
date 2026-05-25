#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/buffer_ops.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using namespace pulp::audio;
namespace ops = pulp::audio::buffer_ops;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// ────────────────────────────────────────────────────────────────────────
// macOS plan item 1.6 — BufferOps SIMD-backed helpers.
//
// These tests pin the audio-friendly semantics on top of the
// `pulp::runtime::simd` primitives: constant gain, gain ramps,
// hard clipping, min/max + magnitude.
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("buffer_ops::apply_gain scales every sample in a span", "[audio][buffer_ops]") {
    std::vector<float> s(128);
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<float>(i);

    ops::apply_gain(std::span<float>{s}, 0.5f);

    for (std::size_t i = 0; i < s.size(); ++i) {
        REQUIRE_THAT(s[i], WithinAbs(static_cast<float>(i) * 0.5f, 1e-6f));
    }
}

TEST_CASE("buffer_ops::apply_gain is a no-op on an empty span", "[audio][buffer_ops]") {
    std::vector<float> s;
    REQUIRE_NOTHROW(ops::apply_gain(std::span<float>{s}, 0.5f));
    REQUIRE(s.empty());
}

TEST_CASE("buffer_ops::apply_gain walks every channel of a BufferView", "[audio][buffer_ops]") {
    Buffer<float> buf(3, 64);
    for (std::size_t ch = 0; ch < 3; ++ch) {
        for (std::size_t i = 0; i < 64; ++i) {
            buf.channel(ch)[i] = static_cast<float>(ch + 1) * 0.25f;
        }
    }

    ops::apply_gain(buf.view(), 2.0f);

    REQUIRE_THAT(buf.channel(0)[0], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(buf.channel(1)[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buf.channel(2)[63], WithinAbs(1.5f, 1e-6f));
}

TEST_CASE("buffer_ops::apply_gain_ramp hits endpoints exactly", "[audio][buffer_ops]") {
    std::vector<float> s(8, 1.0f);
    ops::apply_gain_ramp(std::span<float>{s}, 0.0f, 1.0f);

    REQUIRE_THAT(s[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(s[7], WithinAbs(1.0f, 1e-6f));
    // Midpoint check — step is 1/7, so s[3] = 1 * (3/7).
    REQUIRE_THAT(s[3], WithinAbs(3.0f / 7.0f, 1e-6f));
}

TEST_CASE("buffer_ops::apply_gain_ramp on 1-sample span uses start gain", "[audio][buffer_ops]") {
    std::vector<float> s = {2.0f};
    ops::apply_gain_ramp(std::span<float>{s}, 0.25f, 999.0f);
    REQUIRE_THAT(s[0], WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("buffer_ops::apply_gain_ramp empty span is a no-op", "[audio][buffer_ops]") {
    std::vector<float> s;
    REQUIRE_NOTHROW(ops::apply_gain_ramp(std::span<float>{s}, 0.0f, 1.0f));
}

TEST_CASE("buffer_ops::apply_gain_ramp click-free: adjacent samples step uniformly",
          "[audio][buffer_ops]") {
    // Produce a known impulse, ramp 0→1 over 1024 samples, then verify
    // the per-sample delta is constant within float tolerance.
    constexpr std::size_t N = 1024;
    std::vector<float> s(N, 1.0f);
    ops::apply_gain_ramp(std::span<float>{s}, 0.0f, 1.0f);

    const float expected_step = 1.0f / static_cast<float>(N - 1);
    for (std::size_t i = 1; i < N; ++i) {
        REQUIRE_THAT(s[i] - s[i - 1], WithinAbs(expected_step, 1e-6f));
    }
}

TEST_CASE("buffer_ops::clip bounds samples to [lo, hi]", "[audio][buffer_ops]") {
    std::vector<float> s = {-2.0f, -0.5f, 0.0f, 0.5f, 2.0f};
    ops::clip(std::span<float>{s}, -1.0f, 1.0f);
    REQUIRE_THAT(s[0], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(s[1], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(s[2], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(s[3], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(s[4], WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("buffer_ops::clip on BufferView clips every channel", "[audio][buffer_ops]") {
    Buffer<float> buf(2, 4);
    buf.channel(0)[0] = -3.0f; buf.channel(0)[1] = 3.0f;
    buf.channel(0)[2] = 0.5f;  buf.channel(0)[3] = -0.5f;
    buf.channel(1)[0] = -2.0f; buf.channel(1)[1] = 2.0f;
    buf.channel(1)[2] = 0.0f;  buf.channel(1)[3] = 1.0f;

    ops::clip(buf.view(), -1.0f, 1.0f);

    REQUIRE_THAT(buf.channel(0)[0], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(buf.channel(0)[1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buf.channel(1)[1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buf.channel(1)[0], WithinAbs(-1.0f, 1e-6f));
}

TEST_CASE("buffer_ops::clip sanitizes NaN to lo (Codex P1 on #2864)",
          "[audio][buffer_ops][regression]") {
    // The doc contract says NaN inputs become `lo`. The earlier impl
    // delegated entirely to simd_clamp / std::clamp which propagate
    // NaN through ordered comparisons, so NaN leaked into downstream
    // audio. The post-fix impl converts NaN → lo before clamping.
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // SIMD-multiple length to exercise the vector path.
    std::vector<float> simd_len(64);
    for (std::size_t i = 0; i < simd_len.size(); ++i) {
        simd_len[i] = (i % 7 == 0) ? nan : 0.0f;
    }
    ops::clip(std::span<float>{simd_len}, -0.5f, 0.5f);
    for (std::size_t i = 0; i < simd_len.size(); ++i) {
        REQUIRE_FALSE(std::isnan(simd_len[i]));
        if (i % 7 == 0) REQUIRE_THAT(simd_len[i], WithinAbs(-0.5f, 1e-6f));
    }

    // Short tail (non-SIMD-multiple) — historically the scalar-tail
    // path was what leaked NaN; pin that here too.
    std::vector<float> short_tail = {nan, 0.0f, nan, 2.0f, nan};
    ops::clip(std::span<float>{short_tail}, -1.0f, 1.0f);
    REQUIRE_THAT(short_tail[0], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(short_tail[1], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(short_tail[2], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(short_tail[3], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(short_tail[4], WithinAbs(-1.0f, 1e-6f));
    for (float v : short_tail) REQUIRE_FALSE(std::isnan(v));
}

TEST_CASE("buffer_ops::find_min_max scans a span", "[audio][buffer_ops]") {
    std::vector<float> s = {-3.0f, 1.5f, 0.0f, -0.25f, 2.75f, -2.0f};
    const auto mm = ops::find_min_max(std::span<const float>{s});
    REQUIRE_THAT(mm.min, WithinAbs(-3.0f, 1e-6f));
    REQUIRE_THAT(mm.max, WithinAbs(2.75f, 1e-6f));
}

TEST_CASE("buffer_ops::find_min_max on empty span returns {0, 0}", "[audio][buffer_ops]") {
    std::vector<float> s;
    const auto mm = ops::find_min_max(std::span<const float>{s});
    REQUIRE_THAT(mm.min, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(mm.max, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("buffer_ops::find_min_max on BufferView reads the right channel",
          "[audio][buffer_ops]") {
    Buffer<float> buf(2, 4);
    buf.channel(0)[0] = -1.0f; buf.channel(0)[1] = 0.5f;
    buf.channel(0)[2] = 0.25f; buf.channel(0)[3] = 0.75f;
    buf.channel(1)[0] = -10.0f; buf.channel(1)[1] = 10.0f;
    buf.channel(1)[2] = 0.0f; buf.channel(1)[3] = 0.0f;

    const auto mm0 = ops::find_min_max(buf.view(), /*channel=*/0);
    REQUIRE_THAT(mm0.min, WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(mm0.max, WithinAbs(0.75f, 1e-6f));

    const auto mm1 = ops::find_min_max(buf.view(), /*channel=*/1);
    REQUIRE_THAT(mm1.min, WithinAbs(-10.0f, 1e-6f));
    REQUIRE_THAT(mm1.max, WithinAbs(10.0f, 1e-6f));
}

TEST_CASE("buffer_ops::find_magnitude returns peak |sample|", "[audio][buffer_ops]") {
    std::vector<float> s = {-3.5f, 1.5f, 0.0f, -0.25f, 2.75f, -2.0f};
    REQUIRE_THAT(ops::find_magnitude(std::span<const float>{s}),
                 WithinAbs(3.5f, 1e-6f));

    std::vector<float> all_zero(64, 0.0f);
    REQUIRE_THAT(ops::find_magnitude(std::span<const float>{all_zero}),
                 WithinAbs(0.0f, 1e-6f));

    std::vector<float> all_positive(64, 0.75f);
    REQUIRE_THAT(ops::find_magnitude(std::span<const float>{all_positive}),
                 WithinAbs(0.75f, 1e-6f));
}

TEST_CASE("buffer_ops::find_magnitude on BufferView", "[audio][buffer_ops]") {
    Buffer<float> buf(1, 3);
    buf.channel(0)[0] = -0.5f;
    buf.channel(0)[1] = 0.25f;
    buf.channel(0)[2] = -2.0f;
    REQUIRE_THAT(ops::find_magnitude(buf.view(), /*channel=*/0),
                 WithinAbs(2.0f, 1e-6f));
}

TEST_CASE("buffer_ops SIMD-equivalence: apply_gain matches scalar baseline",
          "[audio][buffer_ops][simd-equivalence]") {
    constexpr std::size_t N = 257; // odd to exercise the scalar tail.
    std::vector<float> simd_path(N);
    std::vector<float> scalar_path(N);
    for (std::size_t i = 0; i < N; ++i) {
        simd_path[i] = scalar_path[i] = std::sin(0.01f * static_cast<float>(i));
    }

    ops::apply_gain(std::span<float>{simd_path}, 0.375f);
    for (auto& s : scalar_path) s *= 0.375f;

    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE_THAT(simd_path[i], WithinAbs(scalar_path[i], 1e-6f));
    }
}

TEST_CASE("buffer_ops SIMD-equivalence: clip matches scalar baseline",
          "[audio][buffer_ops][simd-equivalence]") {
    constexpr std::size_t N = 257;
    std::vector<float> simd_path(N);
    std::vector<float> scalar_path(N);
    for (std::size_t i = 0; i < N; ++i) {
        const float v = 2.0f * std::sin(0.05f * static_cast<float>(i)); // exceeds [-1, 1]
        simd_path[i] = scalar_path[i] = v;
    }

    ops::clip(std::span<float>{simd_path}, -1.0f, 1.0f);
    for (auto& s : scalar_path) s = std::clamp(s, -1.0f, 1.0f);

    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE_THAT(simd_path[i], WithinAbs(scalar_path[i], 1e-6f));
    }
}

TEST_CASE("buffer_ops SIMD-equivalence: find_min_max matches scalar baseline",
          "[audio][buffer_ops][simd-equivalence]") {
    constexpr std::size_t N = 1031; // prime to exercise tail
    std::vector<float> v(N);
    for (std::size_t i = 0; i < N; ++i) {
        v[i] = std::sin(0.013f * static_cast<float>(i)) * 5.0f;
    }
    const auto mm = ops::find_min_max(std::span<const float>{v});

    float scalar_min = v[0], scalar_max = v[0];
    for (auto s : v) {
        scalar_min = std::min(scalar_min, s);
        scalar_max = std::max(scalar_max, s);
    }
    REQUIRE_THAT(mm.min, WithinAbs(scalar_min, 1e-6f));
    REQUIRE_THAT(mm.max, WithinAbs(scalar_max, 1e-6f));
}
