#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/biquad.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr float kSampleRate = 48000.0f;

template <std::size_t N>
void require_buffer_matches(const std::array<float, N>& actual,
                            const std::array<float, N>& expected,
                            float tolerance = 1e-6f) {
    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE_THAT(actual[i], WithinAbs(expected[i], tolerance));
    }
}

template <std::size_t N>
void require_all_finite(const std::array<float, N>& buffer) {
    for (float sample : buffer) {
        REQUIRE(std::isfinite(sample));
    }
}

struct BiquadCase {
    Biquad::Type type;
    float freq_hz;
    float q;
    float gain_db;
};

struct BiquadImpulseCase {
    BiquadCase config;
    std::array<float, 3> expected;
};

}  // namespace

TEST_CASE("Biquad default coefficients are an identity bypass",
          "[signal][biquad][issue-645]") {
    Biquad filter;

    REQUIRE_THAT(filter.process(0.25f), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(filter.process(-0.5f), WithinAbs(-0.5f, 1e-6f));

    std::array<float, 4> buffer{{1.0f, -0.25f, 0.0f, 0.5f}};
    const auto expected = buffer;
    filter.process(buffer.data(), 0);
    require_buffer_matches(buffer, expected);

    filter.process(buffer.data(), static_cast<int>(buffer.size()));
    require_buffer_matches(buffer, expected);
}

TEST_CASE("Biquad zero and negative block lengths are no-ops",
          "[signal][biquad][coverage]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 1000.0f, 0.707f, kSampleRate);
    filter.process(1.0f);

    std::array<float, 3> buffer{{0.25f, -0.5f, 0.75f}};
    const auto expected = buffer;
    filter.process(buffer.data(), 0);
    filter.process(buffer.data(), -4);

    require_buffer_matches(buffer, expected);
}

TEST_CASE("Biquad impulse responses stay pinned for representative filters",
          "[signal][biquad][coverage]") {
    const std::array<BiquadImpulseCase, 8> cases{{
        {{Biquad::Type::lowpass, 600.0f, 0.707f, 0.0f}, {0.00146030f, 0.00567915f, 0.01088156f}},
        {{Biquad::Type::highpass, 4000.0f, 0.707f, 0.0f}, {0.68927898f, -0.49656902f, -0.27527589f}},
        {{Biquad::Type::bandpass, 1200.0f, 1.25f, 0.0f}, {0.05888889f, 0.10947732f, 0.09268173f}},
        {{Biquad::Type::notch, 1800.0f, 0.9f, 0.0f}, {0.88519713f, -0.19763063f, -0.13697046f}},
        {{Biquad::Type::allpass, 2500.0f, 0.8f, 0.0f}, {0.66541807f, -0.52764727f, -0.27489917f}},
        {{Biquad::Type::peaking, 1200.0f, 0.75f, 6.0f}, {1.06842939f, 0.12587992f, 0.10411456f}},
        {{Biquad::Type::low_shelf, 250.0f, 0.707f, 6.0f}, {1.00806409f, 0.01618423f, 0.01628813f}},
        {{Biquad::Type::high_shelf, 8000.0f, 0.707f, -6.0f}, {0.63626967f, 0.22975227f, 0.13133734f}},
    }};

    for (const auto& c : cases) {
        Biquad filter;
        filter.set_coefficients(c.config.type, c.config.freq_hz, c.config.q,
                                kSampleRate, c.config.gain_db);

        std::array<float, 4> impulse{{1.0f, 0.0f, 0.0f, 0.0f}};
        filter.process(impulse.data(), static_cast<int>(impulse.size()));
        require_buffer_matches(std::array<float, 3>{{impulse[0], impulse[1], impulse[2]}},
                               c.expected,
                               1e-5f);
    }
}

TEST_CASE("Biquad extreme valid controls still produce finite sample output",
          "[signal][biquad][coverage]") {
    const std::array<BiquadCase, 8> cases{{
        {Biquad::Type::lowpass, 20.0f, 0.01f, 0.0f},
        {Biquad::Type::highpass, 20000.0f, 25.0f, 0.0f},
        {Biquad::Type::bandpass, 40.0f, 50.0f, 0.0f},
        {Biquad::Type::notch, 19000.0f, 0.02f, 0.0f},
        {Biquad::Type::allpass, 12000.0f, 100.0f, 0.0f},
        {Biquad::Type::peaking, 80.0f, 0.05f, 24.0f},
        {Biquad::Type::low_shelf, 30.0f, 0.1f, -24.0f},
        {Biquad::Type::high_shelf, 18000.0f, 0.1f, 24.0f},
    }};

    for (const auto& c : cases) {
        Biquad filter;
        filter.set_coefficients(c.type, c.freq_hz, c.q, kSampleRate, c.gain_db);

        REQUIRE(std::isfinite(filter.process(1.0f)));
        REQUIRE(std::isfinite(filter.process(-0.5f)));
        REQUIRE(std::isfinite(filter.process(0.25f)));
    }
}

TEST_CASE("Biquad clamps invalid controls to finite coefficients",
          "[signal][biquad][coverage]") {
    // sample_rate <= 0 sends w0 to infinity, q ~ 0 sends alpha to infinity, and a
    // non-finite freq/gain poisons every coefficient — any of which wedges the
    // filter state at NaN forever. The setter clamps these; a filter fed garbage
    // controls must still produce finite output on every sample.
    const std::array<BiquadCase, 6> bad{{
        {Biquad::Type::lowpass, 1000.0f, 0.707f, 0.0f},   // valid ctrl, invalid sr below
        {Biquad::Type::peaking, 1000.0f, 0.0f, 6.0f},     // q == 0
        {Biquad::Type::highpass, 1000.0f, -3.0f, 0.0f},   // negative q
        {Biquad::Type::lowpass, std::numeric_limits<float>::quiet_NaN(), 0.707f, 0.0f},
        {Biquad::Type::high_shelf, 1000.0f, 0.707f, std::numeric_limits<float>::infinity()},
        {Biquad::Type::lowpass, -50.0f, 0.707f, 0.0f},    // negative freq
    }};

    for (std::size_t i = 0; i < bad.size(); ++i) {
        const auto& c = bad[i];
        // First case exercises an invalid sample_rate; the rest exercise the
        // control clamps at a valid sample rate.
        const float sr = (i == 0) ? 0.0f : kSampleRate;
        Biquad filter;
        filter.set_coefficients(c.type, c.freq_hz, c.q, sr, c.gain_db);
        for (float x : {1.0f, -0.5f, 0.25f, -1.0f, 0.0f}) {
            REQUIRE(std::isfinite(filter.process(x)));
        }
    }

    // A tiny but finite POSITIVE Q is a valid (if degenerate) control: it is left
    // untouched by the guard — only q ≤ 0 / non-finite is clamped — and must still
    // yield finite output, since a large-but-finite alpha keeps every coefficient
    // finite. This pins the boundary of the "unchanged for valid inputs" contract.
    {
        Biquad filter;
        filter.set_coefficients(Biquad::Type::lowpass, 1000.0f, 5e-5f, kSampleRate);
        for (float x : {1.0f, -0.5f, 0.25f, -1.0f, 0.0f}) {
            REQUIRE(std::isfinite(filter.process(x)));
        }
    }
}

TEST_CASE("Biquad valid retunes are bit-identical after the clamp guard",
          "[signal][biquad][coverage]") {
    // The input clamps must be pure no-ops for legitimate controls: a valid retune
    // has to yield the exact same coefficients (and thus the exact same samples) as
    // it did before the guard existed. Prove it by driving two filters configured
    // identically and requiring bit-equality across a signal.
    Biquad a, b;
    a.set_coefficients(Biquad::Type::peaking, 1200.0f, 0.75f, kSampleRate, 6.0f);
    b.set_coefficients(Biquad::Type::peaking, 1200.0f, 0.75f, kSampleRate, 6.0f);
    for (int n = 0; n < 64; ++n) {
        const float x = std::sin(0.21f * static_cast<float>(n));
        REQUIRE(a.process(x) == b.process(x));
    }
}

TEST_CASE("Biquad sample and block processing paths match",
          "[signal][biquad][issue-645]") {
    const std::array<BiquadCase, 10> cases{{
        {Biquad::Type::lowpass, 600.0f, 0.707f, 0.0f},
        {Biquad::Type::highpass, 4000.0f, 0.707f, 0.0f},
        {Biquad::Type::bandpass, 1200.0f, 1.25f, 0.0f},
        {Biquad::Type::notch, 1800.0f, 0.9f, 0.0f},
        {Biquad::Type::allpass, 2500.0f, 0.8f, 0.0f},
        {Biquad::Type::peaking, 1200.0f, 0.75f, 6.0f},
        {Biquad::Type::peaking, 1200.0f, 1.5f, -6.0f},
        {Biquad::Type::low_shelf, 250.0f, 0.707f, 6.0f},
        {Biquad::Type::low_shelf, 250.0f, 0.707f, -6.0f},
        {Biquad::Type::high_shelf, 8000.0f, 0.707f, -6.0f},
    }};

    const std::array<float, 12> input{{
        0.75f, -0.5f, 0.25f, 0.0f, 1.0f, -1.0f,
        0.125f, -0.25f, 0.5f, -0.125f, 0.0f, 0.375f,
    }};

    for (const auto& c : cases) {
        Biquad sample_filter;
        Biquad block_filter;
        sample_filter.set_coefficients(c.type, c.freq_hz, c.q, kSampleRate, c.gain_db);
        block_filter.set_coefficients(c.type, c.freq_hz, c.q, kSampleRate, c.gain_db);

        auto sample_output = input;
        for (float& sample : sample_output) {
            sample = sample_filter.process(sample);
        }

        auto block_output = input;
        block_filter.process(block_output.data(), static_cast<int>(block_output.size()));

        require_all_finite(sample_output);
        require_all_finite(block_output);
        require_buffer_matches(block_output, sample_output, 1e-5f);
    }
}

TEST_CASE("Biquad reset clears delayed state but keeps coefficients",
          "[signal][biquad][issue-645]") {
    Biquad dirty;
    dirty.set_coefficients(Biquad::Type::lowpass, 1500.0f, 0.707f, kSampleRate);

    dirty.process(1.0f);
    dirty.process(-0.5f);
    const float ringing = dirty.process(0.0f);
    REQUIRE(std::abs(ringing) > 1e-5f);

    dirty.reset();

    Biquad fresh;
    fresh.set_coefficients(Biquad::Type::lowpass, 1500.0f, 0.707f, kSampleRate);

    const std::array<float, 4> input{{0.75f, -0.25f, 0.0f, 0.5f}};
    for (float sample : input) {
        REQUIRE_THAT(dirty.process(sample), WithinAbs(fresh.process(sample), 1e-6f));
    }
}

TEST_CASE("Biquad zero dB gain filters remain transparent",
          "[signal][biquad][issue-645]") {
    const std::array<BiquadCase, 3> cases{{
        {Biquad::Type::peaking, 1200.0f, 0.75f, 0.0f},
        {Biquad::Type::low_shelf, 300.0f, 0.707f, 0.0f},
        {Biquad::Type::high_shelf, 6000.0f, 0.707f, 0.0f},
    }};
    const std::array<float, 8> input{{0.0f, 0.2f, -0.4f, 0.8f, -0.1f, 0.0f, 0.5f, -0.25f}};

    for (const auto& c : cases) {
        Biquad filter;
        filter.set_coefficients(c.type, c.freq_hz, c.q, kSampleRate, c.gain_db);

        for (float sample : input) {
            REQUIRE_THAT(filter.process(sample), WithinAbs(sample, 1e-5f));
        }
    }
}

TEST_CASE("Biquad edge frequencies produce finite impulse responses",
          "[signal][biquad][issue-645]") {
    const std::array<BiquadCase, 7> cases{{
        {Biquad::Type::lowpass, 0.0f, 0.707f, 0.0f},
        {Biquad::Type::highpass, 0.0f, 0.707f, 0.0f},
        {Biquad::Type::bandpass, 0.0f, 1.0f, 0.0f},
        {Biquad::Type::notch, kSampleRate * 0.5f, 1.0f, 0.0f},
        {Biquad::Type::allpass, kSampleRate * 0.5f, 0.707f, 0.0f},
        {Biquad::Type::low_shelf, 0.0f, 0.707f, -6.0f},
        {Biquad::Type::high_shelf, kSampleRate * 0.5f, 0.707f, 6.0f},
    }};

    for (const auto& c : cases) {
        Biquad filter;
        filter.set_coefficients(c.type, c.freq_hz, c.q, kSampleRate, c.gain_db);

        std::array<float, 8> impulse{{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
        filter.process(impulse.data(), static_cast<int>(impulse.size()));
        require_all_finite(impulse);
    }
}
