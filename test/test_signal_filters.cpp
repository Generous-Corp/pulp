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
#include <cstdint>
#include <cstring>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ── TptFilter ────────────────────────────────────────────────────────────────

TEST_CASE("TptFilter owns invalid sample-rate normalization", "[signal][filter][tpt]") {
    for (const float sample_rate : {
             0.0f,
             -1.0f,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
         }) {
        TptFilter filter;
        filter.prepare(sample_rate);
        filter.set_cutoff(1.0f);

        REQUIRE(filter.sample_rate() == TptFilter::kMinSampleRate);
        REQUIRE(filter.cutoff() == TptFilter::kMinCutoffHz);
        REQUIRE(std::isfinite(filter.process_lowpass(0.25f)));
    }

    TptFilter64 filter64;
    filter64.prepare(-std::numeric_limits<double>::infinity());
    filter64.set_cutoff(1.0);
    REQUIRE(filter64.sample_rate() == TptFilter64::kMinSampleRate);
    REQUIRE(std::isfinite(filter64.process_lowpass(0.25)));
}

TEST_CASE("TptFilter reclamps its retained cutoff when the rate falls", "[signal][filter][tpt]") {
    TptFilter filter;
    filter.prepare(48000.0f);
    filter.set_cutoff(10000.0f);

    filter.prepare(TptFilter::kMinSampleRate);

    REQUIRE_THAT(filter.cutoff(),
                 WithinAbs(TptFilter::kMinSampleRate * TptFilter::kMaxCutoffFraction, 1.0e-6f));
    REQUIRE(std::isfinite(filter.process_lowpass(0.25f)));
}

TEST_CASE("TptFilter64 calculates coefficients at double precision",
          "[signal][filter][tpt][precision]") {
    constexpr double sample_rate = 192000.0;
    constexpr double cutoff = 12345.678901234;
    TptFilter64 filter;
    filter.prepare(sample_rate);
    filter.set_cutoff(cutoff);

    const double tangent = std::tan(std::numbers::pi_v<double> * cutoff / sample_rate);
    const double expected_first_sample = tangent / (1.0 + tangent);

    REQUIRE_THAT(filter.process_lowpass(1.0), WithinAbs(expected_first_sample, 1.0e-15));
}

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
// (fast_math.hpp): FastMath::tanh_accurate by DEFAULT, opt-in Padé
// FastMath::tanh, and exact std::tanh through process_reference(). The
// references below reproduce the process loop with each saturator so the tests
// can (a) bound how far the default moves the filter from exact tanh and (b)
// pin how far the Padé path would — regardless of which way the gate is
// compiled.
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
    return ladder_reference(in, sr, fc, res, [](float v) { return FastMath::tanh(v); });
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
    REQUIRE(max_err_2 < 1e-6f);    // ~-133 dBFS — inaudible
    REQUIRE(max_err_4 < 2e-5f);    // ~-94 dBFS  — Padé error inside |x|<4
    REQUIRE(max_err_full < 7e-4f); // ~-63 dBFS  — the +/-4 hard-clamp seam (worst case)
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
#if PULP_SIGNAL_FAST_LADDER_TANH
    // Opt-in fast build: track the Padé reference, and pin how far it strays
    // from the exact filter (the character delta the gate protects against).
    float max_fast_dev = 0.0f;
    prefix.clear();
    ladder.reset();
    for (float s : drive) {
        prefix.push_back(s);
        const float got = ladder.process(s);
        max_fast_dev = std::max(max_fast_dev,
                                std::abs(got - ladder_reference_fast_tanh(prefix, kSr, kFc, kRes)));
    }
    REQUIRE(max_fast_dev < 1e-3f); // tracks the Padé reference
    REQUIRE(max_abs_dev > 1e-5f);  // and is provably NOT the exact filter
#else
    // Default build: within a few times the saturator's 3.0e-7 bound of the
    // exact filter at +10 dBFS drive, far inside the Padé path's ~1e-3 seam.
    REQUIRE(max_abs_dev < 4e-6f);
#endif
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
                 WithinAbs(ladder_reference_fast_tanh(drive, kSr, kFc, kRes), 1e-3f));
    REQUIRE(std::abs(reference_output - efficient_output) > 1e-5f);
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

// ── Block paths and denormal tails (TptFilter, Svf) ─────────────────────────

namespace {

std::vector<float> filters_busy_signal(std::size_t n, float sample_rate) {
    std::vector<float> s(n);
    std::uint32_t lcg = 0x13579bdu;
    for (std::size_t i = 0; i < n; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = static_cast<float>(lcg >> 8) / 16777216.0f - 0.5f;
        const float t = static_cast<float>(i) / sample_rate;
        s[i] = 0.5f * std::sin(6.2831853f * 220.0f * t) + 0.3f * std::sin(6.2831853f * 3150.0f * t) +
               0.2f * noise;
    }
    return s;
}

std::vector<int> filters_split_schedule(std::size_t total) {
    static constexpr int kPattern[] = {1, 63, 64, 65, 7, 128, 333, 1024, 2, 512, 31};
    std::vector<int> blocks;
    std::size_t done = 0;
    for (std::size_t k = 0; done < total; ++k) {
        const int b = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(kPattern[k % std::size(kPattern)]), total - done));
        blocks.push_back(b);
        done += static_cast<std::size_t>(b);
    }
    return blocks;
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

}  // namespace

TEST_CASE("TptFilter block paths are bit-identical to the sample paths across split blocks",
          "[signal][filter][tpt][determinism]") {
    constexpr float sr = 48000.0f;
    const auto input = filters_busy_signal(8192, sr);
    const auto schedule = filters_split_schedule(input.size());

    for (const float cutoff : {20.0f, 1000.0f, 0.24f * sr, 0.45f * sr}) {
        INFO("cutoff " << cutoff);
        auto run = [&](auto sample_fn, auto block_fn) {
            TptFilter by_sample;
            TptFilter by_block;
            by_sample.prepare(sr);
            by_block.prepare(sr);
            std::vector<float> expected = input;
            std::vector<float> actual = input;
            std::size_t pos = 0;
            int k = 0;
            for (int block : schedule) {
                // Modulate the cutoff at every block boundary on both paths.
                const float c = cutoff * (1.0f + 0.25f * static_cast<float>(k++ % 3));
                by_sample.set_cutoff(c);
                by_block.set_cutoff(c);
                for (int i = 0; i < block; ++i)
                    expected[pos + static_cast<std::size_t>(i)] =
                        sample_fn(by_sample, expected[pos + static_cast<std::size_t>(i)]);
                block_fn(by_block, actual.data() + pos, block);
                pos += static_cast<std::size_t>(block);
            }
            return same_bits(actual, expected);
        };
        CHECK(run([](TptFilter& f, float x) { return f.process_lowpass(x); },
                  [](TptFilter& f, float* p, int n) { f.process_lowpass(p, n); }));
        CHECK(run([](TptFilter& f, float x) { return f.process_highpass(x); },
                  [](TptFilter& f, float* p, int n) { f.process_highpass(p, n); }));
        CHECK(run([](TptFilter& f, float x) { return f.process_allpass(x); },
                  [](TptFilter& f, float* p, int n) { f.process_allpass(p, n); }));
        CHECK(run([](TptFilter& f, float x) { return f.process(x).lowpass; },
                  [](TptFilter& f, float* p, int n) { f.process_lowpass(p, n); }));
    }
}

TEST_CASE("TptFilter tracks a double-precision trapezoidal reference",
          "[signal][filter][tpt][precision]") {
    // The recursion is s' = s + 2g (x - s) with lowpass g (x - s) + s; this
    // is the same filter evaluated in double. The float path must stay within
    // a few float ulps of full scale of it, which bounds any rearrangement of
    // the update to well below -120 dBFS.
    for (const float sr : {44100.0f, 48000.0f, 96000.0f}) {
        for (const float cutoff : {20.0f, 1000.0f, 0.24f * sr, 0.45f * sr}) {
            INFO("sr " << sr << " cutoff " << cutoff);
            TptFilter filter;
            filter.prepare(sr);
            filter.set_cutoff(cutoff);
            const double g = std::tan(std::numbers::pi_v<double> * static_cast<double>(cutoff) /
                                      static_cast<double>(sr));
            const double gd = g / (1.0 + g);
            const auto input = filters_busy_signal(16384, sr);
            auto output = input;
            filter.process_lowpass(output.data(), static_cast<int>(output.size()));
            double state = 0.0;
            double worst = 0.0;
            for (std::size_t i = 0; i < input.size(); ++i) {
                const double d = static_cast<double>(input[i]) - state;
                const double lp = gd * d + state;
                state += 2.0 * gd * d;
                worst = std::max(worst, std::abs(lp - static_cast<double>(output[i])));
            }
            CHECK(worst < 1.0e-6);
        }
    }
}

TEST_CASE("TptFilter block path flushes a decaying tail without subnormal residency",
          "[signal][filter][tpt][denormal]") {
    using namespace pulp::test::denormal_tail;
    if (flush_to_zero_active())
        SKIP("this thread flushes subnormals to zero; the tail counts would be vacuous");
    constexpr std::size_t kTotal = std::size_t{1} << 18;
    constexpr float sr = 48000.0f;

    // Negative control: the unsnapped recursion at 5 Hz sits in a subnormal
    // limit cycle for the rest of the buffer.
    TptFilter probe;
    probe.prepare(sr);
    probe.set_cutoff(5.0f);
    const float g = probe.process_lowpass(1.0f); // first lowpass sample == g
    float state = 0.0f;
    const auto control = impulse_tail(
        [&](float* x, int n) {
            for (int i = 0; i < n; ++i) {
                const float d = x[i] - state;
                x[i] = g * d + state;
                state = 2.0f * g * d + state;
            }
        },
        kTotal, 512);
    REQUIRE(control.subnormal_outputs > kTotal / 2);
    REQUIRE(control.nonzero_end == kTotal);

    for (std::size_t block : {std::size_t{1}, std::size_t{64}, std::size_t{4096}, kTotal}) {
        INFO("block " << block);
        TptFilter filter;
        filter.prepare(sr);
        filter.set_cutoff(5.0f);
        const auto tail =
            impulse_tail([&](float* x, int n) { filter.process_lowpass(x, n); }, kTotal, block);
        REQUIRE(tail.subnormal_outputs == 0);
        REQUIRE(tail.nonzero_end > 0);
        REQUIRE(tail.nonzero_end < kTotal / 4);
    }

    // Near a quarter of the sample rate the pole is close to zero; at most one
    // chunk of subnormal output before exact zero.
    TptFilter fast;
    fast.prepare(sr);
    fast.set_cutoff(0.2f * sr);
    const auto tail = impulse_tail([&](float* x, int n) { fast.process_lowpass(x, n); }, kTotal, kTotal);
    REQUIRE(tail.subnormal_outputs <= static_cast<std::size_t>(TptFilter::kStateSnapInterval));
    REQUIRE(tail.nonzero_end < 1024);
}

TEST_CASE("Svf block path is bit-identical to the sample path for every mode",
          "[signal][svf][determinism]") {
    constexpr float sr = 48000.0f;
    const auto input = filters_busy_signal(8192, sr);
    const auto schedule = filters_split_schedule(input.size());
    for (const auto mode : {Svf::Mode::lowpass, Svf::Mode::highpass, Svf::Mode::bandpass,
                            Svf::Mode::notch}) {
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

TEST_CASE("Svf block path applies a mode change from the next block",
          "[signal][svf]") {
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
        const auto tail = impulse_tail([&](float* x, int n) { filter.process(x, n); }, kTotal, block);
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
    // range (the exhaustive sweep is a private receipt).
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
    CHECK(worst < 3.5e-7);
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
            CHECK(worst < 1.0e-6f);
            CHECK(crossings_fast == crossings_exact);
        }
    }
}

// ── Voice banks ─────────────────────────────────────────────────────────────

TEST_CASE("LadderBank voices are bit-identical to independent LadderFilters",
          "[signal][ladder][bank][determinism]") {
    constexpr std::size_t kVoices = 16;
    LadderBank<kVoices> bank;
    std::array<LadderFilter, kVoices> scalar;
    bank.set_sample_rate(48000.0f);
    for (std::size_t v = 0; v < kVoices; ++v) {
        scalar[v].set_sample_rate(48000.0f);
        scalar[v].set_frequency(300.0f + 250.0f * static_cast<float>(v));
        scalar[v].set_resonance(0.1f * static_cast<float>(v % 11));
        bank.set_frequency(v, 300.0f + 250.0f * static_cast<float>(v));
        bank.set_resonance(v, 0.1f * static_cast<float>(v % 11));
    }
    std::size_t mismatches = 0;
    for (int n = 0; n < 12000; ++n) {
        if (n == 4000) {
            // Retune and reset single voices mid-stream on both sides.
            scalar[3].set_frequency(5000.0f);
            bank.set_frequency(3, 5000.0f);
            scalar[7].set_resonance(1.0f);
            bank.set_resonance(7, 1.0f);
            scalar[9].reset();
            bank.reset(9);
        }
        std::array<float, kVoices> in{};
        for (std::size_t v = 0; v < kVoices; ++v)
            in[v] = 2.5f * std::sin(0.013f * static_cast<float>(n) * static_cast<float>(v + 1));
        std::array<float, kVoices> out{};
        bank.process(in.data(), out.data());
        for (std::size_t v = 0; v < kVoices; ++v) {
            const float expected = scalar[v].process(in[v]);
            mismatches += std::memcmp(&expected, &out[v], sizeof(float)) != 0;
        }
    }
    CHECK(mismatches == 0);
    CHECK(bank.stage_coefficient(3) == LadderFilter::stage_coefficient(5000.0f, 48000.0f));
}

TEST_CASE("LadderBank block processing matches per-sample bank processing",
          "[signal][ladder][bank]") {
    constexpr std::size_t kVoices = 4;
    LadderBank<kVoices> by_block;
    LadderBank<kVoices> by_sample;
    for (auto* b : {&by_block, &by_sample}) {
        b->set_sample_rate(44100.0f);
        for (std::size_t v = 0; v < kVoices; ++v) {
            b->set_frequency(v, 800.0f * static_cast<float>(v + 1));
            b->set_resonance(v, 0.6f);
        }
    }
    std::array<std::vector<float>, kVoices> buffers;
    std::array<float*, kVoices> pointers{};
    for (std::size_t v = 0; v < kVoices; ++v) {
        buffers[v].resize(500);
        for (std::size_t n = 0; n < 500; ++n)
            buffers[v][n] = std::sin(0.02f * static_cast<float>(n * (v + 1)));
        pointers[v] = buffers[v].data();
    }
    auto expected = buffers;
    by_block.process_block(pointers.data(), 500);
    for (std::size_t n = 0; n < 500; ++n) {
        std::array<float, kVoices> frame{};
        for (std::size_t v = 0; v < kVoices; ++v) frame[v] = expected[v][n];
        by_sample.process(frame.data(), frame.data());
        for (std::size_t v = 0; v < kVoices; ++v) expected[v][n] = frame[v];
    }
    for (std::size_t v = 0; v < kVoices; ++v)
        CHECK(std::memcmp(buffers[v].data(), expected[v].data(), 500 * sizeof(float)) == 0);
}

TEST_CASE("SvfBank voices are bit-identical to independent Svfs in every mode",
          "[signal][svf][bank][determinism]") {
    constexpr std::size_t kVoices = 16;
    SvfBank<kVoices> bank;
    std::array<Svf, kVoices> scalar;
    for (std::size_t v = 0; v < kVoices; ++v) {
        const auto mode = static_cast<Svf::Mode>(v % 4);
        const float f = 200.0f + 300.0f * static_cast<float>(v);
        const float q = 0.5f + 0.3f * static_cast<float>(v % 5);
        scalar[v].configure(mode, 48000.0f, f, q);
        bank.configure(v, mode, 48000.0f, f, q);
    }
    std::size_t mismatches = 0;
    for (int n = 0; n < 12000; ++n) {
        if (n == 5000) {
            scalar[2].set_mode(Svf::Mode::notch);
            bank.set_mode(2, Svf::Mode::notch);
            scalar[5].set_frequency(9000.0f);
            bank.set_frequency(5, 9000.0f);
            scalar[11].set_resonance(8.0f);
            bank.set_resonance(11, 8.0f);
            scalar[13].reset();
            bank.reset(13);
        }
        std::array<float, kVoices> in{};
        for (std::size_t v = 0; v < kVoices; ++v)
            in[v] = std::sin(0.017f * static_cast<float>(n) * static_cast<float>(v + 1));
        std::array<float, kVoices> out{};
        bank.process(in.data(), out.data());
        for (std::size_t v = 0; v < kVoices; ++v) {
            const float expected = scalar[v].process(in[v]);
            mismatches += std::memcmp(&expected, &out[v], sizeof(float)) != 0;
        }
    }
    CHECK(mismatches == 0);
    const auto c = bank.coefficients(5);
    const auto d = Svf::design(48000.0f, 9000.0f, 0.5f + 0.3f * 0.0f);
    CHECK(c.g == d.g);
}
