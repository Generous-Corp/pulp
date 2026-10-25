#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <pulp/signal/signal.hpp>
#include "support/denormal_tail.hpp"
#include "support/filter_block_fixtures.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

using namespace pulp::signal;
using namespace pulp::test::filter_block;
using Catch::Matchers::WithinAbs;

// ── Biquad ───────────────────────────────────────────────────────────────────

TEST_CASE("Biquad lowpass attenuates high frequencies", "[signal][biquad]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 200.0f, 0.707f, 44100.0f);

    // Generate 10kHz sine (should be attenuated)
    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 10000.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 100)
            sum_sq += output * output; // Skip transient
    }
    float rms = std::sqrt(sum_sq / 4310.0f);
    REQUIRE(rms < 0.01f); // Should be heavily attenuated
}

TEST_CASE("Biquad lowpass passes low frequencies", "[signal][biquad]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 5000.0f, 0.707f, 44100.0f);

    // Generate 100Hz sine (should pass through)
    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 200)
            sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms > 0.6f); // Should pass through mostly
}

TEST_CASE("Biquad highpass attenuates low frequencies", "[signal][biquad]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::highpass, 5000.0f, 0.707f, 44100.0f);

    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 200)
            sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms < 0.05f);
}

TEST_CASE("Biquad reset clears state", "[signal][biquad]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 1000.0f, 0.707f, 44100.0f);

    filter.process(1.0f);
    filter.process(0.5f);
    filter.reset();

    // After reset, should behave as if freshly initialized
    float out = filter.process(0.0f);
    REQUIRE_THAT(out, WithinAbs(0.0, 0.001));
}

TEST_CASE("Biquad coefficient variants process finite impulse buffers", "[signal][biquad]") {
    struct Case {
        Biquad::Type type;
        float gain_db;
    };

    const std::array<Case, 6> cases{{
        {Biquad::Type::bandpass, 0.0f},
        {Biquad::Type::notch, 0.0f},
        {Biquad::Type::allpass, 0.0f},
        {Biquad::Type::peaking, 6.0f},
        {Biquad::Type::low_shelf, 6.0f},
        {Biquad::Type::high_shelf, -6.0f},
    }};

    for (const auto& c : cases) {
        Biquad filter;
        filter.set_coefficients(c.type, 1200.0f, 0.707f, 48000.0f, c.gain_db);

        float impulse[] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        filter.process(impulse, 8);

        for (float sample : impulse) {
            REQUIRE(std::isfinite(sample));
        }
    }
}

// ── SVF ──────────────────────────────────────────────────────────────────────

TEST_CASE("SVF lowpass attenuates high frequencies", "[signal][svf]") {
    Svf filter;
    filter.set_sample_rate(44100.0f);
    filter.set_frequency(200.0f);
    filter.set_resonance(0.707f);
    filter.set_mode(Svf::Mode::lowpass);

    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 10000.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 100)
            sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4310.0f);
    REQUIRE(rms < 0.01f);
}

TEST_CASE("SVF highpass passes high frequencies", "[signal][svf]") {
    Svf filter;
    filter.set_sample_rate(44100.0f);
    filter.set_frequency(200.0f);
    filter.set_resonance(0.707f);
    filter.set_mode(Svf::Mode::highpass);

    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 5000.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 200)
            sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms > 0.5f);
}

TEST_CASE("SVF covers bandpass notch buffer and reset paths", "[signal][svf][issue-645]") {
    Svf filter;
    filter.set_sample_rate(48000.0f);
    filter.set_frequency(1000.0f);
    filter.set_resonance(0.9f);

    filter.set_mode(Svf::Mode::bandpass);
    float band = filter.process(1.0f);
    REQUIRE(std::isfinite(band));

    filter.set_mode(Svf::Mode::notch);
    float notched = filter.process(1.0f);
    REQUIRE(std::isfinite(notched));

    float buffer[] = {1.0f, 0.5f, 0.0f, -0.5f};
    filter.process(buffer, 4);
    for (float sample : buffer) {
        REQUIRE(std::isfinite(sample));
    }

    filter.reset();
    filter.set_mode(Svf::Mode::lowpass);
    float after_reset = filter.process(0.0f);
    REQUIRE_THAT(after_reset, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("SVF ignores nonpositive buffer lengths", "[signal][svf]") {
    Svf filter;
    filter.set_sample_rate(48000.0f);
    filter.set_frequency(1000.0f);
    filter.set_mode(Svf::Mode::bandpass);

    float samples[] = {0.25f, -0.5f, 0.75f};
    filter.process(samples, 0);
    filter.process(samples, -3);

    REQUIRE_THAT(samples[0], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(samples[2], WithinAbs(0.75f, 1e-6f));
}

// ── LadderFilter ─────────────────────────────────────────────────────────────

TEST_CASE("LadderFilter lowpass behavior", "[signal][ladder]") {
    LadderFilter ladder;
    ladder.set_sample_rate(44100.0f);
    ladder.set_frequency(200.0f);
    ladder.set_resonance(0.3f);

    // High frequency should be attenuated
    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 10000.0f * i / 44100.0f);
        float output = ladder.process(input);
        if (i > 200)
            sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms < 0.05f); // 24dB/oct should heavily attenuate
}

// The float ladder's saturator is gated by PULP_SIGNAL_FAST_LADDER_TANH
// (fast_math.hpp): FastMath::tanh_accurate by DEFAULT, opt-in
// FastMath::tanh_minimax, and exact std::tanh through process_reference(). The
// references below reproduce the process loop with each saturator so the tests
// can (a) bound how far the default moves the filter from exact tanh and (b)
// pin how far the efficient path would — regardless of which way the gate is
// compiled. The Lambert-fraction reference is the control that shows the
// clamp seam the efficient path no longer has.
namespace {
template <typename Sat>
float ladder_reference(const std::vector<float>& in, float sr, float fc, float res, Sat sat) {
    float stage[4] = {};
    const float g = 1.0f - std::exp(-2.0f * 3.14159265f * fc / sr);
    float last = 0.0f;
    for (float input : in) {
        const float feedback = res * 4.0f * (stage[3] - input * 0.5f);
        const float x = input - feedback;
        for (int i = 0; i < 4; ++i) {
            const float prev = i > 0 ? stage[i - 1] : x;
            stage[i] += g * (sat(prev) - sat(stage[i]));
        }
        last = stage[3];
    }
    return last;
}
float ladder_reference_std_tanh(const std::vector<float>& in, float sr, float fc, float res) {
    return ladder_reference(in, sr, fc, res, [](float v) { return std::tanh(v); });
}
[[maybe_unused]] float ladder_reference_accurate_tanh(const std::vector<float>& in, float sr,
                                                      float fc, float res) {
    return ladder_reference(in, sr, fc, res, [](float v) { return FastMath::tanh_accurate(v); });
}
[[maybe_unused]] float ladder_reference_fast_tanh(const std::vector<float>& in, float sr, float fc,
                                                  float res) {
    return ladder_reference(in, sr, fc, res, [](float v) { return FastMath::tanh_minimax(v); });
}
// The Lambert [7/6] Pade with its output saturated at |x| = 4: a 6.7e-4 step
// there, the seam a saturator must not have. Kept as the control below.
float seamed_lambert_tanh(float v) {
    if (v < -4.0f)
        return -1.0f;
    if (v > 4.0f)
        return 1.0f;
    const float x2 = v * v;
    return v * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) /
           (135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f)));
}
float ladder_reference_lambert_tanh(const std::vector<float>& in, float sr, float fc, float res) {
    return ladder_reference(in, sr, fc, res, seamed_lambert_tanh);
}
} // namespace

// Regression guard on the Padé approximation itself. This is what makes the
// fast path safe to keep as an opt-in: a future edit to FastMath::tanh that
// widened its error would break this before it could reach anyone's ears.
TEST_CASE("FastMath::tanh stays within its pinned error bound of std::tanh",
          "[signal][ladder][fast_math]") {
    // Central region the ladder lives in at normal levels — essentially float
    // epsilon (~-130 dBFS).
    float max_err_2 = 0.0f, max_err_4 = 0.0f, max_err_full = 0.0f;
    for (int i = 0; i <= 200000; ++i) {
        const float x = -16.0f + 32.0f * static_cast<float>(i) / 200000.0f;
        const float e = std::abs(FastMath::tanh(x) - static_cast<float>(std::tanh(x)));
        max_err_full = std::max(max_err_full, e);
        if (std::abs(x) <= 4.0f)
            max_err_4 = std::max(max_err_4, e);
        if (std::abs(x) <= 2.0f)
            max_err_2 = std::max(max_err_2, e);
    }
    REQUIRE(max_err_2 < 1e-6f);      // ~-133 dBFS — inaudible
    REQUIRE(max_err_4 < 2e-5f);      // ~-94 dBFS  — Padé error inside |x|<4
    REQUIRE(max_err_full < 9.7e-5f); // ~-80 dBFS  — at the argument clamp (worst case)
}

// The default float ladder must track the EXACT std::tanh filter to within
// the accurate saturator's bound even when overdriven. If someone flips the
// default to the Padé path, this fails — the fidelity guarantee is pinned, not
// implicit.
TEST_CASE("LadderFilter default saturator tracks exact std::tanh", "[signal][ladder]") {
    constexpr float kSr = 48000.0f, kFc = 1200.0f, kRes = 0.7f;
    LadderFilter ladder;
    ladder.set_sample_rate(kSr);
    ladder.set_frequency(kFc);
    ladder.set_resonance(kRes);

    // Hard drive (amp 3) pushes stage args past 4 — where the two saturators
    // diverge most — so this is a strict test of "the default really is libm".
    std::vector<float> drive;
    drive.reserve(2048);
    for (int i = 0; i < 2048; ++i)
        drive.push_back(3.0f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / kSr));

    float max_abs_dev = 0.0f;
    std::vector<float> prefix;
    prefix.reserve(drive.size());
    for (float s : drive) {
        prefix.push_back(s);
        const float got = ladder.process(s);
        const float exact = ladder_reference_std_tanh(prefix, kSr, kFc, kRes);
        max_abs_dev = std::max(max_abs_dev, std::abs(got - exact));
        REQUIRE(std::isfinite(got));
    }
    // The bound depends on which saturator the build made the default
    // (PULP_SIGNAL_LADDER_SATURATION / PULP_SIGNAL_FAST_LADDER_TANH).
    if constexpr (kDefaultLadderSaturation == LadderSaturation::realtime_efficient) {
        // Track the efficient reference, and pin how far it strays from the
        // exact filter: under the Lambert seam this was ~5e-4.
        float max_fast_dev = 0.0f;
        prefix.clear();
        ladder.reset();
        for (float s : drive) {
            prefix.push_back(s);
            const float got = ladder.process(s);
            max_fast_dev = std::max(
                max_fast_dev, std::abs(got - ladder_reference_fast_tanh(prefix, kSr, kFc, kRes)));
        }
        REQUIRE(max_fast_dev < 1e-5f); // tracks the efficient reference
        REQUIRE(max_abs_dev > 0.0f);   // and is provably NOT the exact filter
        REQUIRE(max_abs_dev < 2e-5f);  // with no clamp seam
    } else if constexpr (kDefaultLadderSaturation == LadderSaturation::accurate) {
        // Within a few times the saturator's 3.0e-7 bound of the exact filter
        // at +10 dBFS drive.
        REQUIRE(max_abs_dev < 4e-6f);
    } else {
        // Exact std::tanh: the reference loop reproduces it to round-off.
        REQUIRE(max_abs_dev < 1e-6f);
    }
}

TEST_CASE("LadderFilter explicit saturation profiles ignore the build default",
          "[signal][ladder][fast_math]") {
    constexpr float kSr = 48000.0f, kFc = 1200.0f, kRes = 0.7f;
    LadderFilter reference;
    LadderFilter efficient;
    for (auto* ladder : {&reference, &efficient}) {
        ladder->set_sample_rate(kSr);
        ladder->set_frequency(kFc);
        ladder->set_resonance(kRes);
    }

    std::vector<float> drive;
    drive.reserve(2048);
    float reference_output = 0.0f;
    float efficient_output = 0.0f;
    for (int i = 0; i < 2048; ++i) {
        const float sample =
            3.0f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / kSr);
        drive.push_back(sample);
        reference_output = reference.process_reference(sample);
        efficient_output = efficient.process_realtime_efficient(sample);
    }

    REQUIRE_THAT(reference_output,
                 WithinAbs(ladder_reference_std_tanh(drive, kSr, kFc, kRes), 1e-5f));
    REQUIRE_THAT(efficient_output,
                 WithinAbs(ladder_reference_fast_tanh(drive, kSr, kFc, kRes), 1e-5f));
    REQUIRE(reference_output != efficient_output);
}

TEST_CASE("LadderFilter efficient profile has no clamp seam under hard drive",
          "[signal][ladder][fast_math]") {
    // Amplitude 3 at high resonance pushes stage arguments past |x| = 4 every
    // cycle, which is where a Lambert fraction saturated at 4 steps to its
    // clamp. The same prefix run through that form is the control: it must show the
    // seam this stimulus exists to provoke, or a small deviation proves nothing.
    constexpr float kSr = 48000.0f, kFc = 1200.0f, kRes = 0.9f;
    LadderFilter exact;
    LadderFilter efficient;
    for (auto* ladder : {&exact, &efficient}) {
        ladder->set_sample_rate(kSr);
        ladder->set_frequency(kFc);
        ladder->set_resonance(kRes);
    }
    const auto input = [&](int n) {
        return 3.0f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(n) / kSr);
    };
    float worst = 0.0f;
    float lambert_worst = 0.0f;
    std::vector<float> prefix;
    for (int i = 0; i < 2048; ++i) {
        const float e = exact.process_reference(input(i));
        worst = std::max(worst, std::abs(efficient.process_realtime_efficient(input(i)) - e));
        prefix.push_back(input(i));
        if (i % 64 == 63)
            lambert_worst = std::max(
                lambert_worst, std::abs(ladder_reference_lambert_tanh(prefix, kSr, kFc, kRes) - e));
    }
    INFO("efficient " << worst << " lambert " << lambert_worst);
    REQUIRE(lambert_worst > 1e-4f);
    REQUIRE(worst < 2e-5f);
}

TEST_CASE("ladder_tanh_profiled boolean form keeps its exact and Pade meanings",
          "[signal][ladder][fast_math]") {
    // At this drive the three saturators disagree, so a boolean argument that
    // resolved to the wrong enumerator (true taken as accurate, value 1) fails.
    constexpr float kDrive = 2.5f;
    const float exact = ladder_tanh_profiled<LadderSaturation::exact>(kDrive);
    const float accurate = ladder_tanh_profiled<LadderSaturation::accurate>(kDrive);
    const float efficient = ladder_tanh_profiled<LadderSaturation::realtime_efficient>(kDrive);
    REQUIRE(efficient != accurate);
    REQUIRE(efficient != exact);

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    CHECK(ladder_tanh_profiled<false>(kDrive) == exact);
    CHECK(ladder_tanh_profiled<true>(kDrive) == efficient);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
}

TEST_CASE("LadderFilter double instantiation still uses libm tanh", "[signal][ladder]") {
    // FastMath::tanh is float-only; the double path must remain exact enough
    // that a strict tanh-based reference matches to double tolerance.
    LadderFilter64 ladder;
    ladder.set_sample_rate(48000.0);
    ladder.set_frequency(1000.0);
    ladder.set_resonance(0.5);

    double stage[4] = {};
    // The header builds g from SampleType{3.14159265f} — a *float* literal even
    // for the double instantiation. Mirror that exactly or the tolerance below
    // is measuring pi, not tanh.
    const double kPi = static_cast<double>(3.14159265f);
    const double g = 1.0 - std::exp(-2.0 * kPi * 1000.0 / 48000.0);
    for (int n = 0; n < 256; ++n) {
        const double input = 0.5 * std::sin(0.05 * n);
        const double got = ladder.process(input);

        const double feedback = 0.5 * 4.0 * (stage[3] - input * 0.5);
        const double x = input - feedback;
        for (int i = 0; i < 4; ++i) {
            const double prev = i > 0 ? stage[i - 1] : x;
            stage[i] += g * (std::tanh(prev) - std::tanh(stage[i]));
        }
        REQUIRE_THAT(got, WithinAbs(stage[3], 1e-12));
    }
}

TEST_CASE("LadderFilter resets buffer state and clamps resonance inputs",
          "[signal][ladder][issue-645]") {
    LadderFilter ladder;
    ladder.set_sample_rate(48000.0f);
    ladder.set_frequency(1200.0f);
    ladder.set_resonance(-2.0f);

    float impulse[] = {1.0f, 0.0f, 0.0f, 0.0f};
    ladder.process(impulse, 4);
    for (float sample : impulse) {
        REQUIRE(std::isfinite(sample));
    }

    ladder.reset();
    REQUIRE_THAT(ladder.process(0.0f), WithinAbs(0.0f, 1e-6f));

    ladder.set_resonance(3.0f);
    ladder.set_frequency(8000.0f);
    for (int i = 0; i < 32; ++i) {
        const float sample = (i == 0) ? 0.75f : 0.0f;
        REQUIRE(std::isfinite(ladder.process(sample)));
    }

    float unchanged[] = {0.25f, -0.25f};
    ladder.process(unchanged, 0);
    REQUIRE_THAT(unchanged[0], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(unchanged[1], WithinAbs(-0.25f, 1e-6f));
}

TEST_CASE("LadderFilter reset restores fresh-filter impulse response", "[signal][ladder]") {
    auto impulse_response = [] {
        LadderFilter filter;
        filter.set_sample_rate(48000.0f);
        filter.set_frequency(1600.0f);
        filter.set_resonance(0.65f);

        std::array<float, 6> response{};
        response[0] = filter.process(1.0f);
        for (std::size_t i = 1; i < response.size(); ++i)
            response[i] = filter.process(0.0f);
        return response;
    };

    const auto fresh = impulse_response();

    LadderFilter reused;
    reused.set_sample_rate(48000.0f);
    reused.set_frequency(1600.0f);
    reused.set_resonance(0.65f);
    for (int i = 0; i < 16; ++i)
        reused.process(0.25f);

    reused.reset();
    REQUIRE_THAT(reused.process(1.0f), WithinAbs(fresh[0], 1e-6f));
    for (std::size_t i = 1; i < fresh.size(); ++i) {
        INFO("impulse sample " << i);
        REQUIRE_THAT(reused.process(0.0f), WithinAbs(fresh[i], 1e-6f));
    }
}

TEST_CASE("LadderFilter ignores nonpositive buffer lengths", "[signal][ladder]") {
    LadderFilter ladder;
    ladder.set_sample_rate(44100.0f);
    ladder.set_frequency(1200.0f);

    float samples[] = {0.25f, -0.5f, 0.75f};
    ladder.process(samples, 0);
    ladder.process(samples, -8);

    REQUIRE_THAT(samples[0], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(samples[2], WithinAbs(0.75f, 1e-6f));
}

// ── LinkwitzRiley ────────────────────────────────────────────────────────────

TEST_CASE("LinkwitzRiley splits into low and high bands", "[signal][lr]") {
    LinkwitzRiley lr;
    lr.set_frequency(1000.0f, 44100.0f);

    // 200Hz signal should be mostly in the low band
    float low_energy = 0, high_energy = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 200.0f * i / 44100.0f);
        auto split = lr.process(input);
        if (i > 200) { // Skip transient
            low_energy += split.low * split.low;
            high_energy += split.high * split.high;
        }
    }
    REQUIRE(low_energy > high_energy * 10.0f); // Low band should dominate

    // 5kHz signal should be mostly in the high band
    lr.reset();
    low_energy = 0;
    high_energy = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 5000.0f * i / 44100.0f);
        auto split = lr.process(input);
        if (i > 200) {
            low_energy += split.low * split.low;
            high_energy += split.high * split.high;
        }
    }
    REQUIRE(high_energy > low_energy * 10.0f); // High band should dominate
}

TEST_CASE("LinkwitzRiley reset clears history while preserving coefficients",
          "[signal][lr][issue-645]") {
    auto impulse_response = [](LinkwitzRiley& lr) {
        std::vector<LinkwitzRiley::BandSplit> response;
        response.reserve(24);
        for (int i = 0; i < 24; ++i) {
            response.push_back(lr.process(i == 0 ? 1.0f : 0.0f));
        }
        return response;
    };

    LinkwitzRiley fresh;
    fresh.set_frequency(1200.0f, 48000.0f);
    auto expected = impulse_response(fresh);

    LinkwitzRiley reused;
    reused.set_frequency(1200.0f, 48000.0f);
    for (int i = 0; i < 128; ++i) {
        auto split = reused.process((i % 3 == 0) ? 0.75f : -0.25f);
        REQUIRE(std::isfinite(split.low));
        REQUIRE(std::isfinite(split.high));
    }

    reused.reset();
    auto actual = impulse_response(reused);

    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        REQUIRE_THAT(actual[i].low, WithinAbs(expected[i].low, 1e-6f));
        REQUIRE_THAT(actual[i].high, WithinAbs(expected[i].high, 1e-6f));
    }
}

TEST_CASE("LinkwitzRiley cutoff boundary processing stays finite", "[signal][lr][issue-645]") {
    for (float cutoff : {0.0f, 20.0f, 20000.0f, 24000.0f}) {
        LinkwitzRiley lr;
        lr.set_frequency(cutoff, 48000.0f);

        for (int i = 0; i < 64; ++i) {
            float input = (i == 0) ? 1.0f : ((i % 2 == 0) ? 0.125f : -0.125f);
            auto split = lr.process(input);
            INFO("cutoff=" << cutoff << " sample=" << i);
            REQUIRE(std::isfinite(split.low));
            REQUIRE(std::isfinite(split.high));
        }
    }
}

TEST_CASE("LinkwitzRiley retuning after active history stays finite", "[signal][lr]") {
    LinkwitzRiley lr;
    lr.set_frequency(300.0f, 48000.0f);
    for (int i = 0; i < 32; ++i) {
        (void)lr.process((i % 2 == 0) ? 0.5f : -0.5f);
    }

    lr.set_frequency(2400.0f, 48000.0f);
    for (int i = 0; i < 16; ++i) {
        auto split = lr.process(i == 0 ? 1.0f : 0.0f);
        REQUIRE(std::isfinite(split.low));
        REQUIRE(std::isfinite(split.high));
    }
}

// ── Svf block path and denormal tail ────────────────────────────────────────

TEST_CASE("Svf block path is bit-identical to the sample path for every mode",
          "[signal][svf][determinism]") {
    constexpr float sr = 48000.0f;
    const auto input = filters_busy_signal(8192, sr);
    const auto schedule = filters_split_schedule(input.size());
    for (const auto mode :
         {Svf::Mode::lowpass, Svf::Mode::highpass, Svf::Mode::bandpass, Svf::Mode::notch}) {
        INFO("mode " << static_cast<int>(mode));
        Svf by_sample;
        Svf by_block;
        by_sample.configure(mode, sr, 1000.0f, 0.7071f);
        by_block.configure(mode, sr, 1000.0f, 0.7071f);
        std::vector<float> expected = input;
        std::vector<float> actual = input;
        std::size_t pos = 0;
        int k = 0;
        for (int block : schedule) {
            if (k++ % 4 == 1) {
                const float f = 200.0f * static_cast<float>(1 + k % 7);
                const float q = 0.5f + static_cast<float>(k % 5);
                by_sample.set_frequency(f);
                by_block.set_frequency(f);
                by_sample.set_resonance(q);
                by_block.set_resonance(q);
            }
            for (int i = 0; i < block; ++i)
                expected[pos + static_cast<std::size_t>(i)] =
                    by_sample.process(expected[pos + static_cast<std::size_t>(i)]);
            by_block.process(actual.data() + pos, block);
            pos += static_cast<std::size_t>(block);
        }
        CHECK(same_bits(actual, expected));
    }
}

TEST_CASE("Svf block path applies a mode change from the next block", "[signal][svf]") {
    constexpr float sr = 48000.0f;
    const auto input = filters_busy_signal(256, sr);
    Svf by_sample;
    Svf by_block;
    by_sample.configure(Svf::Mode::lowpass, sr, 1500.0f, 2.0f);
    by_block.configure(Svf::Mode::lowpass, sr, 1500.0f, 2.0f);
    std::vector<float> expected = input;
    std::vector<float> actual = input;
    const Svf::Mode order[] = {Svf::Mode::lowpass, Svf::Mode::highpass, Svf::Mode::bandpass,
                               Svf::Mode::notch};
    for (int b = 0; b < 4; ++b) {
        by_sample.set_mode(order[b]);
        by_block.set_mode(order[b]);
        for (int i = 0; i < 64; ++i)
            expected[static_cast<std::size_t>(b * 64 + i)] =
                by_sample.process(expected[static_cast<std::size_t>(b * 64 + i)]);
        by_block.process(actual.data() + b * 64, 64);
    }
    CHECK(same_bits(actual, expected));
}

TEST_CASE("Svf block path flushes a decaying tail without subnormal residency",
          "[signal][svf][denormal]") {
    using namespace pulp::test::denormal_tail;
    if (flush_to_zero_active())
        SKIP("this thread flushes subnormals to zero; the tail counts would be vacuous");
    constexpr std::size_t kTotal = std::size_t{1} << 18;

    // Negative control: the same TPT recursion with no snap.
    const float g = std::tan(3.14159265f * 20.0f / 48000.0f);
    const float k = 1.0f / 0.7071f;
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    float ic1 = 0.0f;
    float ic2 = 0.0f;
    const auto control = impulse_tail(
        [&](float* x, int n) {
            for (int i = 0; i < n; ++i) {
                const float v3 = x[i] - ic2;
                const float v1 = a1 * ic1 + a2 * v3;
                const float v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                x[i] = v2;
            }
        },
        kTotal, 512);
    REQUIRE(control.subnormal_outputs > kTotal / 2);

    for (std::size_t block : {std::size_t{1}, std::size_t{64}, std::size_t{4096}, kTotal}) {
        INFO("block " << block);
        Svf filter;
        filter.configure(Svf::Mode::lowpass, 48000.0f, 20.0f, 0.7071f);
        const auto tail =
            impulse_tail([&](float* x, int n) { filter.process(x, n); }, kTotal, block);
        REQUIRE(tail.subnormal_outputs == 0);
        REQUIRE(tail.nonzero_end > 0);
        // A one-sample block snaps every sample, as the sample path does; the
        // chunked snap must still bring the tail to exact zero.
        if (block > 1)
            REQUIRE(tail.nonzero_end < kTotal / 8);
    }
}

TEST_CASE("FastMath::tanh_accurate stays within its pinned bound of std::tanh",
          "[signal][ladder][fast_math]") {
    // Every 64th positive float: the bound is 3.0e-7 absolute over the whole
    // range with fused multiply-add and 3.6e-7 without (the no-contract suite
    // checks the latter on its own).
    double worst = 0.0;
    double worst_small_rel = 0.0;
    for (std::uint32_t bits = 0; bits < 0x7f800000u; bits += 64) {
        float x;
        std::memcpy(&x, &bits, sizeof x);
        const double exact = std::tanh(static_cast<double>(x));
        const double got = FastMath::tanh_accurate(x);
        worst = std::max(worst, std::abs(got - exact));
        if (x > 0.0f && x < 0.05f)
            worst_small_rel = std::max(worst_small_rel, std::abs(got - exact) / exact);
        // Odd symmetry is exact, not approximate.
        REQUIRE(FastMath::tanh_accurate(-x) == -FastMath::tanh_accurate(x));
    }
    CHECK(worst < 3.6e-7);
    // Unit slope at the origin: small signals see the same loop gain as tanh.
    CHECK(worst_small_rel < 3.0e-7);
    CHECK(FastMath::tanh_accurate(9.5f) == 1.0f);
    CHECK(FastMath::tanh_accurate(-9.5f) == -1.0f);
    CHECK(FastMath::tanh_accurate(1.0e30f) == 1.0f);
    CHECK(FastMath::tanh_accurate(-1.0e30f) == -1.0f);
    CHECK(FastMath::tanh_accurate(std::numeric_limits<float>::infinity()) == 1.0f);
    CHECK(FastMath::tanh_accurate(-std::numeric_limits<float>::infinity()) == -1.0f);
    // A NaN must propagate, not turn into a full-scale constant.
    CHECK(std::isnan(FastMath::tanh_accurate(std::numeric_limits<float>::quiet_NaN())));
    CHECK(std::isnan(FastMath::tanh_accurate(-std::numeric_limits<float>::quiet_NaN())));
    CHECK(std::signbit(FastMath::tanh_accurate(-0.0f)));
}

TEST_CASE("LadderFilter default saturator preserves resonance and drive behavior",
          "[signal][ladder][parity]") {
    // Against process_reference() (exact std::tanh stages): ringing frequency,
    // decay, and the driven waveform must match to well below -120 dBFS at
    // normal levels.
    constexpr float kSr = 48000.0f;
    for (const float fc : {500.0f, 2000.0f}) {
        for (const float res : {0.0f, 0.9f, 1.0f}) {
            INFO("fc " << fc << " res " << res);
            LadderFilter fast;
            LadderFilter exact;
            for (auto* l : {&fast, &exact}) {
                l->set_sample_rate(kSr);
                l->set_frequency(fc);
                l->set_resonance(res);
            }
            float worst = 0.0f;
            int crossings_fast = 0;
            int crossings_exact = 0;
            float prev_fast = 0.0f;
            float prev_exact = 0.0f;
            for (int i = 0; i < 48000; ++i) {
                // A kick, then a 0 dBFS two-tone, then silence to ring out.
                float x = i < 32 ? 2.0f : 0.0f;
                if (i >= 8000 && i < 24000)
                    x = 0.6f * std::sin(6.2831853f * 220.0f * static_cast<float>(i) / kSr) +
                        0.4f * std::sin(6.2831853f * 3150.0f * static_cast<float>(i) / kSr);
                const float a = fast.process(x);
                const float b = exact.process_reference(x);
                worst = std::max(worst, std::abs(a - b));
                if (i > 1000 && i < 8000) {
                    crossings_fast += prev_fast < 0.0f && a >= 0.0f;
                    crossings_exact += prev_exact < 0.0f && b >= 0.0f;
                }
                prev_fast = a;
                prev_exact = b;
            }
            // The bound is the build default's (see kDefaultLadderSaturation):
            // accurate and exact stay below -120 dBFS; realtime_efficient's
            // 3.8e-6 saturator error, amplified by the resonant recursion,
            // stays below -90 dBFS.
            const float bound = kDefaultLadderSaturation == LadderSaturation::realtime_efficient
                                    ? 3.0e-5f
                                    : 1.0e-6f;
            CHECK(worst < bound);
            CHECK(crossings_fast == crossings_exact);
        }
    }
}

TEST_CASE("LadderFilter default saturator holds its documented bound up to +20 dBFS drive",
          "[signal][ladder][parity]") {
    // The grid behind LadderSaturation::accurate's documented bound: cutoff
    // 300 Hz - 6 kHz, resonance 0 - 1, sine input 110 Hz - 7 kHz at 48 kHz,
    // pointwise against process_reference() (exact std::tanh stages). Worst
    // cases measured with Apple clang and with GCC and glibc: -124.5 dBFS at
    // 0 dBFS, -107.3 dBFS at +10 dBFS, -101.9 dBFS at +20 dBFS (high
    // resonance, low input, high cutoff). Each limit sits about 2 dB above
    // the measurement.
    struct Drive {
        float amplitude;
        float limit;
    };
    constexpr float kSr = 48000.0f;
    for (const Drive drive : {Drive{1.0f, 7.9e-7f},     // -122 dBFS at 0 dBFS
                              Drive{3.1623f, 5.6e-6f},  // -105 dBFS at +10 dBFS
                              Drive{10.0f, 1.0e-5f}}) { // -100 dBFS at +20 dBFS
        float worst = 0.0f;
        for (const float f0 : {110.0f, 440.0f, 2500.0f, 7000.0f}) {
            for (const float res : {0.0f, 0.5f, 0.9f, 1.0f}) {
                for (const float fc : {300.0f, 1500.0f, 6000.0f}) {
                    LadderFilter fast;
                    LadderFilter exact;
                    for (auto* l : {&fast, &exact}) {
                        l->set_sample_rate(kSr);
                        l->set_frequency(fc);
                        l->set_resonance(res);
                    }
                    const double w = 2.0 * std::numbers::pi * static_cast<double>(f0) / kSr;
                    for (int i = 0; i < 48000; ++i) {
                        const float x = static_cast<float>(static_cast<double>(drive.amplitude) *
                                                           std::sin(w * static_cast<double>(i)));
                        worst =
                            std::max(worst, std::abs(fast.process(x) - exact.process_reference(x)));
                    }
                }
            }
        }
        INFO("amplitude " << drive.amplitude << " worst " << worst);
        CHECK(worst < drive.limit);
    }
}
