#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/biquad.hpp>
#include <array>
#include <cmath>
#include <cstddef>

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
