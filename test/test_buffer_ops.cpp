#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/buffer_ops.hpp>

#include "../core/audio/src/peak_rms_kernels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
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

TEST_CASE("buffer_ops::clip sanitizes NaN to lo (#2864)",
          "[audio][buffer_ops][regression]") {
    // The doc contract says NaN inputs become `lo`. simd_clamp / std::clamp
    // propagate NaN through ordered comparisons, so the impl converts
    // NaN → lo before clamping to keep NaN out of downstream audio.
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

// ── measure_peak_rms ────────────────────────────────────────────────────

TEST_CASE("buffer_ops::measure_peak_rms is exact on exactly representable input",
          "[audio][buffer_ops][meter]") {
    // Multiples of 0.25 square and sum exactly in float, so any summation
    // order gives the same bits. 37 samples exercises the 16-lane body and a
    // 5-sample tail; the largest magnitude is negative.
    std::vector<float> v(37);
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = 0.25f * static_cast<float>(i % 9) * ((i % 2) ? -1.0f : 1.0f);
    v[36] = -3.0f;
    double sum = 0.0;
    for (float x : v)
        sum += static_cast<double>(x) * x;
    const auto level = ops::measure_peak_rms(std::span<const float>{v});
    REQUIRE(level.peak == 3.0f);
    REQUIRE(level.rms == std::sqrt(static_cast<float>(sum) / 37.0f));
}

TEST_CASE("buffer_ops::measure_peak_rms finds a negative peak in every lane",
          "[audio][buffer_ops][meter]") {
    // 40 samples: two full 16-lane passes plus a tail. A negative sample of
    // the largest magnitude is placed at each position in turn, so every
    // partial (and its absolute value) has to reach the result.
    for (std::size_t at = 0; at < 40; ++at) {
        std::vector<float> v(40, 0.25f);
        v[at] = -2.0f;
        const auto level = ops::measure_peak_rms(std::span<const float>{v});
        INFO("peak at " << at);
        REQUIRE(level.peak == 2.0f);
        REQUIRE(level.rms == std::sqrt((39.0f * 0.0625f + 4.0f) / 40.0f));
    }
}

TEST_CASE("buffer_ops::measure_peak_rms vector and portable kernels agree bit for bit",
          "[audio][buffer_ops][meter][parity]") {
    namespace detail = pulp::audio::buffer_ops::detail;
#if defined(__GNUC__) || defined(__clang__)
    // On these compilers the vector kernel must be the real one; otherwise
    // the comparison below would be the portable kernel against itself.
    REQUIRE(detail::has_vector_peak_rms_kernel());
#endif
    const float specials[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::signaling_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::denorm_min() * 37.0f,
        -std::numeric_limits<float>::min() * 0.5f, // subnormal
        -0.0f,
    };
    std::vector<std::size_t> sizes;
    for (std::size_t n = 0; n <= 64; ++n)
        sizes.push_back(n);
    for (std::size_t n : {127u, 128u, 129u, 255u, 256u, 257u, 1031u})
        sizes.push_back(n);

    std::uint32_t state = 0x2545F491u;
    const auto next = [&state] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (static_cast<float>(state >> 8) / 16777216.0f - 0.5f) * 3.0f;
    };
    const auto same_bits = [](const ops::PeakRms& a, const ops::PeakRms& b) {
        return std::memcmp(&a.peak, &b.peak, sizeof(float)) == 0 &&
               std::memcmp(&a.rms, &b.rms, sizeof(float)) == 0;
    };

    std::size_t checked = 0;
    for (std::size_t n : sizes) {
        std::vector<float> base(n);
        for (auto& v : base)
            v = next();
        // -1: no special value; otherwise one special at the start of the
        // vector body, one in the middle, and one in the scalar tail.
        for (int special = -1; special < static_cast<int>(std::size(specials)); ++special) {
            std::vector<std::size_t> positions;
            if (special < 0) {
                positions.push_back(n); // sentinel: no placement
            } else if (n > 0) {
                positions = {0, n / 2, n - 1};
                if (n > 16)
                    positions.push_back((n / 16) * 16); // first tail sample
            }
            for (std::size_t at : positions) {
                auto v = base;
                if (at < n)
                    v[at] = specials[special];
                const auto portable = detail::measure_peak_rms_portable(v);
                const auto vector = detail::measure_peak_rms_vector(v);
                const auto dispatched = ops::measure_peak_rms(std::span<const float>{v});
                INFO("n = " << n << ", special = " << special << ", at = " << at
                            << ", portable = " << portable.peak << "/" << portable.rms
                            << ", vector = " << vector.peak << "/" << vector.rms);
                REQUIRE(same_bits(portable, vector));
                REQUIRE(same_bits(dispatched, vector));
                ++checked;
            }
        }
    }
    INFO("vector kernel present: " << detail::has_vector_peak_rms_kernel());
    REQUIRE(checked >= 1900);
}

TEST_CASE("buffer_ops::measure_peak_rms of an empty span is zero", "[audio][buffer_ops][meter]") {
    const auto level = ops::measure_peak_rms(std::span<const float>{});
    REQUIRE(level.peak == 0.0f);
    REQUIRE(level.rms == 0.0f);
}

TEST_CASE("buffer_ops::measure_peak_rms matches a double-precision reference",
          "[audio][buffer_ops][meter]") {
    for (std::size_t n : {1u, 7u, 15u, 16u, 17u, 128u, 1031u}) {
        std::vector<float> v(n);
        float naive_peak = 0.0f;
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            v[i] = 0.8f * std::sin(0.37f * static_cast<float>(i) + 0.1f) +
                   0.15f * std::cos(2.9f * static_cast<float>(i));
            naive_peak = std::max(naive_peak, std::abs(v[i]));
            sum += static_cast<double>(v[i]) * v[i];
        }
        const auto level = ops::measure_peak_rms(std::span<const float>{v});
        INFO("n = " << n);
        REQUIRE(level.peak == naive_peak);
        REQUIRE_THAT(level.rms,
                     WithinRel(static_cast<float>(std::sqrt(sum / static_cast<double>(n))), 1e-6f));
    }
}

TEST_CASE("buffer_ops::measure_peak_rms keeps NaN out of peak but not out of RMS",
          "[audio][buffer_ops][meter][nan]") {
    // Quiet and signaling NaNs, in the vector body and in the scalar tail.
    // A signaling NaN matters separately: IEEE maxNum (std::fmax, FMAXNM)
    // returns NaN for it, so only a compare-select keeps it out of the peak.
    for (const float nan :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::signaling_NaN()}) {
        // 16 is the first tail sample, which lands in partial 0: the partial
        // every combine step keeps, so a NaN there cannot be hidden by the
        // combine and has to be rejected by the per-sample fold itself.
        for (std::size_t at : {0u, 3u, 16u, 18u}) {
            std::vector<float> v(19, 0.5f);
            v[11] = -0.75f;
            v[at] = nan;
            const auto level = ops::measure_peak_rms(std::span<const float>{v});
            INFO("NaN at " << at);
            REQUIRE(level.peak == 0.75f);
            REQUIRE(std::isnan(level.rms));
        }
    }
}

TEST_CASE("buffer_ops::measure_peak_rms reports an infinite sample as infinite",
          "[audio][buffer_ops][meter]") {
    std::vector<float> v(16, 0.1f);
    v[5] = -std::numeric_limits<float>::infinity();
    const auto level = ops::measure_peak_rms(std::span<const float>{v});
    REQUIRE(std::isinf(level.peak));
    REQUIRE(std::isinf(level.rms));
}

TEST_CASE("buffer_ops::measure_peak_rms measures one channel of a BufferView",
          "[audio][buffer_ops][meter]") {
    Buffer<float> buf(2, 64);
    for (std::size_t i = 0; i < 64; ++i) {
        buf.channel(0)[i] = 0.5f;
        buf.channel(1)[i] = (i == 40) ? -2.0f : 0.0f;
    }
    auto view = buf.view();
    const auto left = ops::measure_peak_rms(view, 0);
    REQUIRE(left.peak == 0.5f);
    REQUIRE(left.rms == 0.5f);

    const BufferView<const float> cview = std::as_const(buf).view();
    const auto right = ops::measure_peak_rms(cview, 1);
    REQUIRE(right.peak == 2.0f);
    REQUIRE(right.rms == std::sqrt(4.0f / 64.0f));
}

// ── map_samples ─────────────────────────────────────────────────────────

TEST_CASE("buffer_ops::map_samples maps out of place without touching the input",
          "[audio][buffer_ops][map_samples]") {
    std::vector<float> in(131), out(131, -1.0f);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = 0.01f * static_cast<float>(i) - 0.5f;
    const auto original = in;
    ops::map_samples(in.data(), out.data(), in.size(), [](float x) { return 0.5f * x + 0.25f; });
    REQUIRE(in == original);
    for (std::size_t i = 0; i < in.size(); ++i)
        REQUIRE(out[i] == 0.5f * in[i] + 0.25f);
}

TEST_CASE("buffer_ops::map_samples in place matches out of place bit for bit",
          "[audio][buffer_ops][map_samples]") {
    std::vector<float> in(257);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = std::sin(0.11f * static_cast<float>(i)) * 1.7f;
    const auto fn = [](float x) { return x * 0.70710677f - x * x * 0.125f; };
    std::vector<float> separate(in.size());
    ops::map_samples(in.data(), separate.data(), in.size(), fn);
    std::vector<float> inplace = in;
    ops::map_samples(inplace.data(), inplace.data(), inplace.size(), fn);
    REQUIRE(inplace == separate);
}

TEST_CASE("buffer_ops::map_samples calls the function once per sample in order",
          "[audio][buffer_ops][map_samples]") {
    for (bool in_place : {false, true}) {
        std::vector<float> in(37);
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = static_cast<float>(i);
        std::vector<float> out(in.size());
        float* dst = in_place ? in.data() : out.data();
        std::vector<float> seen;
        float running = 0.0f;
        ops::map_samples(in.data(), dst, in.size(), [&](float x) {
            seen.push_back(x);
            running += x;
            return running; // prefix sum: only correct if samples arrive in order
        });
        INFO("in_place = " << in_place);
        REQUIRE(seen.size() == 37);
        for (std::size_t i = 0; i < 37; ++i) {
            REQUIRE(seen[i] == static_cast<float>(i));
            REQUIRE(dst[i] == static_cast<float>(i * (i + 1) / 2));
        }
    }
}

TEST_CASE("buffer_ops::map_samples with zero samples does nothing",
          "[audio][buffer_ops][map_samples]") {
    int calls = 0;
    float sample = 3.0f;
    ops::map_samples(&sample, &sample, 0, [&](float x) {
        ++calls;
        return x;
    });
    REQUIRE(calls == 0);
    REQUIRE(sample == 3.0f);
}

// ── with_channel_count ──────────────────────────────────────────────────

namespace {
struct ChannelDispatchRecord {
    int calls = 0;
    bool constant = false;
    std::size_t value = 0;
};

template <std::size_t... Counts> ChannelDispatchRecord dispatch_record(std::size_t channels) {
    ChannelDispatchRecord record;
    ops::with_channel_count<Counts...>(channels, [&](auto count) {
        ++record.calls;
        record.constant = !std::is_same_v<decltype(count), std::size_t>;
        record.value = count;
    });
    return record;
}
} // namespace

TEST_CASE("buffer_ops::with_channel_count passes a listed count as a constant",
          "[audio][buffer_ops][channel_dispatch]") {
    for (std::size_t channels : {1u, 2u}) {
        const auto record = dispatch_record<1, 2>(channels);
        INFO("channels = " << channels);
        REQUIRE(record.calls == 1);
        REQUIRE(record.constant);
        REQUIRE(record.value == channels);
    }
}

TEST_CASE("buffer_ops::with_channel_count passes any other count at run time",
          "[audio][buffer_ops][channel_dispatch]") {
    for (std::size_t channels : {0u, 3u, 8u}) {
        const auto record = dispatch_record<1, 2>(channels);
        INFO("channels = " << channels);
        REQUIRE(record.calls == 1);
        REQUIRE_FALSE(record.constant);
        REQUIRE(record.value == channels);
    }
    const auto none_listed = dispatch_record<>(2);
    REQUIRE(none_listed.calls == 1);
    REQUIRE_FALSE(none_listed.constant);
    REQUIRE(none_listed.value == 2);
}
