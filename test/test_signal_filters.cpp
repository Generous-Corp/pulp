#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/signal.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace pulp::signal;
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
        if (i > 100) sum_sq += output * output; // Skip transient
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
        if (i > 200) sum_sq += output * output;
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
        if (i > 200) sum_sq += output * output;
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
        if (i > 100) sum_sq += output * output;
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
        if (i > 200) sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms > 0.5f);
}

TEST_CASE("SVF covers bandpass notch buffer and reset paths",
          "[signal][svf][issue-645]") {
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

TEST_CASE("SVF ignores nonpositive buffer lengths",
          "[signal][svf]") {
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
        if (i > 200) sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms < 0.05f); // 24dB/oct should heavily attenuate
}

// LadderFilterT<float> saturates with FastMath::tanh (Padé, float-only) rather
// than libm; LadderFilterT<double> keeps std::tanh. Pin the resulting deviation
// so a future change to FastMath::tanh cannot silently move the filter's
// non-linearity. The reference below is the exact formulation the float path
// replaced.
namespace {
float ladder_reference_std_tanh(const std::vector<float>& in, float sr, float fc, float res) {
    float stage[4] = {};
    const float g = 1.0f - std::exp(-2.0f * 3.14159265f * fc / sr);
    float last = 0.0f;
    for (float input : in) {
        const float feedback = res * 4.0f * (stage[3] - input * 0.5f);
        const float x = input - feedback;
        for (int i = 0; i < 4; ++i) {
            const float prev = i > 0 ? stage[i - 1] : x;
            stage[i] += g * (std::tanh(prev) - std::tanh(stage[i]));
        }
        last = stage[3];
    }
    return last;
}
}  // namespace

TEST_CASE("LadderFilter fast-tanh path tracks the std::tanh reference",
          "[signal][ladder]") {
    constexpr float kSr = 48000.0f, kFc = 1200.0f, kRes = 0.7f;

    LadderFilter ladder;
    ladder.set_sample_rate(kSr);
    ladder.set_frequency(kFc);
    ladder.set_resonance(kRes);

    // Drive hard enough that stages saturate — that is where the Padé
    // approximation's +/-4 clamp differs most from libm.
    std::vector<float> drive;
    drive.reserve(2048);
    for (int i = 0; i < 2048; ++i)
        drive.push_back(3.0f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / kSr));

    float max_abs_dev = 0.0f;
    std::vector<float> prefix;
    prefix.reserve(drive.size());
    for (float s : drive) {
        prefix.push_back(s);
        const float fast = ladder.process(s);
        const float exact = ladder_reference_std_tanh(prefix, kSr, kFc, kRes);
        max_abs_dev = std::max(max_abs_dev, std::abs(fast - exact));
        REQUIRE(std::isfinite(fast));
    }

    // ~3e-5 per tanh, 8 per sample, accumulated through a resonant feedback
    // loop. Comfortably below the noise floor of any format we emit.
    REQUIRE(max_abs_dev < 1e-2f);
}

TEST_CASE("LadderFilter double instantiation still uses libm tanh",
          "[signal][ladder]") {
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

TEST_CASE("LadderFilter reset restores fresh-filter impulse response",
          "[signal][ladder]") {
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

TEST_CASE("LadderFilter ignores nonpositive buffer lengths",
          "[signal][ladder]") {
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
    low_energy = 0; high_energy = 0;
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

TEST_CASE("LinkwitzRiley cutoff boundary processing stays finite",
          "[signal][lr][issue-645]") {
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

TEST_CASE("LinkwitzRiley retuning after active history stays finite",
          "[signal][lr]") {
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
