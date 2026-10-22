#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

#if defined(_MSC_VER)
#define PULP_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define PULP_TEST_NOINLINE __attribute__((noinline))
#else
#define PULP_TEST_NOINLINE
#endif

PULP_TEST_NOINLINE float runtime_sin_cycles(float phase, FastTrigProfile profile) {
    return FastMath::sin_cycles(phase, profile);
}

#undef PULP_TEST_NOINLINE

float lambert_tanh_through_13(float x) {
    constexpr float odd_denominators[] = {11.0f, 9.0f, 7.0f, 5.0f, 3.0f};
    const float x2 = x * x;
    float fraction = 13.0f;
    for (float denominator : odd_denominators)
        fraction = denominator + x2 / fraction;
    return x / (1.0f + x2 / fraction);
}

std::uint32_t positive_float_ulp_distance(float lhs, float rhs) {
    const auto lhs_bits = std::bit_cast<std::uint32_t>(lhs);
    const auto rhs_bits = std::bit_cast<std::uint32_t>(rhs);
    return lhs_bits > rhs_bits ? lhs_bits - rhs_bits : rhs_bits - lhs_bits;
}

// Exact approximation removed by the landed exp2 correctness fix. Keeping it
// test-local proves the dense oracle rejects the former production behavior.
float prior_pulp_cubic_exp2(float x) noexcept {
    const float integral = std::floor(x);
    const float fractional = x - integral;
    const float polynomial =
        1.0f + fractional * (0.6931472f + fractional * (0.2402265f + fractional * 0.0558015f));
    return polynomial * std::ldexp(1.0f, static_cast<int>(integral));
}

} // namespace

TEST_CASE("FastMath tanh approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::tanh(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(FastMath::tanh(1.0f), WithinAbs(std::tanh(1.0f), 0.001));
    REQUIRE_THAT(FastMath::tanh(-1.0f), WithinAbs(std::tanh(-1.0f), 0.001));
    REQUIRE_THAT(FastMath::tanh(3.0f), WithinAbs(std::tanh(3.0f), 0.01));
    REQUIRE_THAT(FastMath::tanh(-4.0f), WithinAbs(std::tanh(-4.0f), 0.001));
    REQUIRE_THAT(FastMath::tanh(4.0f), WithinAbs(std::tanh(4.0f), 0.001));
    REQUIRE_THAT(FastMath::tanh(-5.0f), WithinAbs(-1.0, 0.001));
    REQUIRE_THAT(FastMath::tanh(5.0f), WithinAbs(1.0, 0.001));
}

TEST_CASE("FastMath tanh matches the reduced Lambert continued fraction", "[signal][fast_math]") {
    constexpr float values[] = {-4.0f, -3.0f, -1.0f, -0.25f, 0.0f, 0.25f, 1.0f, 3.0f, 4.0f};
    for (float value : values) {
        REQUIRE_THAT(FastMath::tanh(value), WithinAbs(lambert_tanh_through_13(value), 1e-6f));
    }
}

TEST_CASE("FastMath tanh clamps exactly beyond approximation range", "[signal][fast_math]") {
    REQUIRE(FastMath::tanh(-100.0f) == -1.0f);
    REQUIRE(FastMath::tanh(-4.0001f) == -1.0f);
    REQUIRE(FastMath::tanh(4.0001f) == 1.0f);
    REQUIRE(FastMath::tanh(100.0f) == 1.0f);
}

TEST_CASE("FastMath sin approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::sin(0.0f), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(FastMath::sin(1.5707963f), WithinAbs(1.0, 0.01)); // pi/2
    REQUIRE_THAT(FastMath::sin(3.1415926f), WithinAbs(0.0, 0.02)); // pi
    REQUIRE_THAT(FastMath::sin(-1.5707963f), WithinAbs(-1.0, 0.01));
}

TEST_CASE("FastMath trigonometry wraps large phases", "[signal][fast_math][issue-645]") {
    constexpr float pi = 3.14159265f;
    constexpr float two_pi = 6.28318530f;

    REQUIRE_THAT(FastMath::sin(two_pi + pi * 0.5f), WithinAbs(1.0f, 0.01f));
    REQUIRE_THAT(FastMath::sin(-two_pi - pi * 0.5f), WithinAbs(-1.0f, 0.01f));
    REQUIRE_THAT(FastMath::cos(-two_pi), WithinAbs(1.0f, 0.01f));
}

TEST_CASE("FastMath bounded-cycle trig profiles meet their float error budgets",
          "[signal][fast_math][trig-profile]") {
    constexpr std::size_t sample_count = 1u << 17;
    double efficient_max_error = 0.0;
    double precise_max_error = 0.0;
    for (std::size_t i = 0; i < sample_count; ++i) {
        const float phase = static_cast<float>(i) / static_cast<float>(sample_count);
        const double expected =
            std::sin(2.0 * std::acos(-1.0) * static_cast<double>(phase));
        efficient_max_error = std::max(
            efficient_max_error,
            std::abs(static_cast<double>(FastMath::sin_cycles<
                         FastTrigProfile::realtime_efficient>(phase)) -
                     expected));
        precise_max_error = std::max(
            precise_max_error,
            std::abs(static_cast<double>(FastMath::sin_cycles<
                         FastTrigProfile::realtime_precise>(phase)) -
                     expected));
    }

    REQUIRE(efficient_max_error <= 1.2e-4);
    REQUIRE(precise_max_error <= 2.5e-7);
}

TEST_CASE("FastMath bounded-cycle runtime dispatch preserves semantic profiles",
          "[signal][fast_math][trig-profile]") {
    constexpr float phases[] = {0.0f, 0.125f, 0.25f, 0.5f, 0.75f, 0.875f};
    for (const float phase : phases) {
        REQUIRE(runtime_sin_cycles(phase, FastTrigProfile::reference) ==
                FastMath::sin_cycles<FastTrigProfile::reference>(phase));
        REQUIRE(runtime_sin_cycles(phase, FastTrigProfile::realtime_efficient) ==
                FastMath::sin_cycles<FastTrigProfile::realtime_efficient>(phase));
        REQUIRE(runtime_sin_cycles(phase, FastTrigProfile::realtime_precise) ==
                FastMath::sin_cycles<FastTrigProfile::realtime_precise>(phase));
    }
    const auto invalid = static_cast<FastTrigProfile>(std::numeric_limits<std::uint8_t>::max());
    REQUIRE(runtime_sin_cycles(0.375f, invalid) ==
            FastMath::sin_cycles<FastTrigProfile::reference>(0.375f));
}

#if defined(__APPLE__) && defined(__clang__)
TEST_CASE("FastMath Apple four-wide precise sine meets the scalar error budget",
          "[signal][fast_math][trig-profile][simd]") {
    constexpr std::size_t sample_count = 1u << 17;
    double maximum = 0.0;
    double scalar_parity_maximum = 0.0;
    for (std::size_t i = 0; i < sample_count; i += 4) {
        const simd_float4 phase = simd_make_float4(
            static_cast<float>(i) / static_cast<float>(sample_count),
            static_cast<float>(i + 1) / static_cast<float>(sample_count),
            static_cast<float>(i + 2) / static_cast<float>(sample_count),
            static_cast<float>(i + 3) / static_cast<float>(sample_count));
        const simd_float4 actual = FastMath::sin_cycles_precise(phase);
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const double expected = std::sin(
                2.0 * std::acos(-1.0) * static_cast<double>(phase[lane]));
            maximum = std::max(
                maximum,
                std::abs(static_cast<double>(actual[lane]) - expected));
            const double scalar = FastMath::sin_cycles<
                FastTrigProfile::realtime_precise>(phase[lane]);
            scalar_parity_maximum = std::max(
                scalar_parity_maximum,
                std::abs(static_cast<double>(actual[lane]) - scalar));
        }
    }
    REQUIRE(maximum <= 2.5e-7);
    REQUIRE(scalar_parity_maximum <= 2.5e-7);
}
#endif

TEST_CASE("FastMath cos approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::cos(0.0f), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(FastMath::cos(1.5707963f), WithinAbs(0.0, 0.02));  // pi/2
    REQUIRE_THAT(FastMath::cos(3.1415926f), WithinAbs(-1.0, 0.02)); // pi
}

TEST_CASE("FastMath exp2 common values", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::exp2(0.0f), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(FastMath::exp2(1.0f), WithinAbs(2.0, 0.01));
    REQUIRE_THAT(FastMath::exp2(3.0f), WithinAbs(8.0, 0.05));
    REQUIRE_THAT(FastMath::exp2(-1.0f), WithinAbs(0.5, 0.01));
    REQUIRE_THAT(FastMath::exp2(-1.5f), WithinAbs(std::exp2(-1.5f), 0.01));
    REQUIRE_THAT(FastMath::exp2(0.5f), WithinAbs(std::sqrt(2.0f), 0.01));
}

TEST_CASE("FastMath exp2 preserves ordering across integer boundaries", "[signal][fast_math]") {
    const float below = FastMath::exp2(1.999f);
    const float exact = FastMath::exp2(2.0f);
    const float above = FastMath::exp2(2.001f);

    REQUIRE(below < exact);
    REQUIRE(exact < above);
    REQUIRE_THAT(exact, WithinAbs(4.0f, 0.01f));
}

TEST_CASE("FastMath exp2 covers fractional octaves across zero", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::exp2(-0.5f), WithinAbs(std::exp2(-0.5f), 0.01f));
    REQUIRE_THAT(FastMath::exp2(1.25f), WithinRel(std::exp2(1.25f), 0.01f));
    REQUIRE_THAT(FastMath::exp2(7.75f), WithinRel(std::exp2(7.75f), 0.01f));
}

TEST_CASE("FastMath exp2 defines exceptional and range-edge results", "[signal][fast_math]") {
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    constexpr auto quiet_nan = std::numeric_limits<float>::quiet_NaN();

    REQUIRE(std::isnan(FastMath::exp2(quiet_nan)));
    REQUIRE(FastMath::exp2(infinity) == infinity);
    REQUIRE(FastMath::exp2(-infinity) == 0.0f);
    REQUIRE_FALSE(std::signbit(FastMath::exp2(-infinity)));
    REQUIRE(FastMath::exp2(128.0f) == infinity);
    REQUIRE(FastMath::exp2(-150.0f) == 0.0f);
    REQUIRE(FastMath::exp2(-149.0f) == std::numeric_limits<float>::denorm_min());
}

TEST_CASE("FastMath exp2 follows the audio callback floating-point mode",
          "[signal][fast_math]") {
    volatile float exponent = -149.0f;
    float standard = 0.0f;
    float actual = 0.0f;
    {
        ScopedFlushDenormals flush_denormals;
        standard = std::exp2(exponent);
        actual = FastMath::exp2(exponent);
    }

    INFO("callback-mode std::exp2(-149)=" << standard
                                          << " FastMath::exp2(-149)=" << actual);
    REQUIRE(actual == standard);
    REQUIRE(actual >= 0.0f);
    REQUIRE_FALSE(std::signbit(actual));
}

TEST_CASE("FastMath exp2 returns every integer power exactly under gradual underflow",
          "[signal][fast_math]") {
    for (int exponent = -149; exponent <= 127; ++exponent)
        REQUIRE(FastMath::exp2(static_cast<float>(exponent)) == std::ldexp(1.0f, exponent));
}

TEST_CASE("FastMath exp2 is continuous and monotonic at integer boundaries",
          "[signal][fast_math]") {
    constexpr auto negative_infinity = -std::numeric_limits<float>::infinity();
    constexpr auto positive_infinity = std::numeric_limits<float>::infinity();

    for (int exponent = -149; exponent <= 127; ++exponent) {
        const float boundary = static_cast<float>(exponent);
        const float below = FastMath::exp2(std::nextafter(boundary, negative_infinity));
        const float exact = FastMath::exp2(boundary);
        const float above = FastMath::exp2(std::nextafter(boundary, positive_infinity));
        REQUIRE(below <= exact);
        REQUIRE(exact <= above);
    }
}

TEST_CASE("FastMath exp2 agrees with a dense double-precision oracle", "[signal][fast_math]") {
    constexpr std::size_t sample_count = 1'000'001;
    constexpr double first = -150.0;
    constexpr double last = 128.0;
    std::uint32_t maximum_ulp_error = 0;
    std::uint32_t prior_maximum_ulp_error = 0;

    for (std::size_t index = 0; index < sample_count; ++index) {
        const float exponent =
            static_cast<float>(first + (last - first) * static_cast<double>(index) /
                                           static_cast<double>(sample_count - 1));
        const double exact = std::exp2(static_cast<double>(exponent));
        const float oracle = static_cast<float>(exact);
        const float actual = FastMath::exp2(exponent);
        maximum_ulp_error =
            std::max(maximum_ulp_error, positive_float_ulp_distance(actual, oracle));
        if (std::isfinite(oracle)) {
            prior_maximum_ulp_error =
                std::max(prior_maximum_ulp_error,
                         positive_float_ulp_distance(prior_pulp_cubic_exp2(exponent), oracle));
        }
    }

    INFO("max ULP error = " << maximum_ulp_error);
    INFO("planted prior approximation max ULP error = " << prior_maximum_ulp_error);
    REQUIRE(maximum_ulp_error <= 1);
    REQUIRE(prior_maximum_ulp_error > 1);
}

TEST_CASE("FastMath exp2 round trips through the standard logarithm where finite and normal",
          "[signal][fast_math]") {
    constexpr std::size_t sample_count = 100'001;
    constexpr float first = -126.0f;
    constexpr float last = 127.0f;
    float maximum_absolute_error = 0.0f;

    for (std::size_t index = 0; index < sample_count; ++index) {
        const float exponent = first + (last - first) * static_cast<float>(index) /
                                           static_cast<float>(sample_count - 1);
        const float roundtrip = std::log2(FastMath::exp2(exponent));
        maximum_absolute_error = std::max(maximum_absolute_error, std::abs(roundtrip - exponent));
    }

    INFO("max exp2/log2 roundtrip absolute error = " << maximum_absolute_error);
    REQUIRE(maximum_absolute_error < 2.0e-5f);
}

TEST_CASE("FastMath exp2 has bounded relative and cents error in the DSP domain",
          "[signal][fast_math]") {
    constexpr std::size_t sample_count = 1'000'001;
    constexpr double first = -126.0;
    constexpr double last = 127.0;
    double maximum_relative_error = 0.0;
    double maximum_cents_error = 0.0;

    for (std::size_t index = 0; index < sample_count; ++index) {
        const float exponent =
            static_cast<float>(first + (last - first) * static_cast<double>(index) /
                                           static_cast<double>(sample_count - 1));
        const double exact = std::exp2(static_cast<double>(exponent));
        const double actual = static_cast<double>(FastMath::exp2(exponent));
        const double ratio = actual / exact;
        maximum_relative_error = std::max(maximum_relative_error, std::abs(ratio - 1.0));
        maximum_cents_error = std::max(maximum_cents_error, std::abs(1200.0 * std::log2(ratio)));
    }

    INFO("max relative error = " << maximum_relative_error);
    INFO("max cents error = " << maximum_cents_error);
    REQUIRE(maximum_relative_error < 1.0e-7);
    REQUIRE(maximum_cents_error < 0.0002);
}

TEST_CASE("FastMath exp2 preserves pitch and audio-rate FM products", "[signal][fast_math]") {
    constexpr double reference_hz = 440.0;
    constexpr double sample_rate = 192'000.0;
    double maximum_pitch_cents_error = 0.0;
    double maximum_fm_phase_error = 0.0;
    bool all_fm_increments_are_positive_and_finite = true;

    for (int cent = -12'000; cent <= 12'000; ++cent) {
        const float octaves = static_cast<float>(cent) / 1200.0f;
        const double expected_hz = reference_hz * std::exp2(static_cast<double>(octaves));
        const double actual_hz = reference_hz * static_cast<double>(FastMath::exp2(octaves));
        maximum_pitch_cents_error = std::max(maximum_pitch_cents_error,
                                             std::abs(1200.0 * std::log2(actual_hz / expected_hz)));
    }

    for (int step = -32'768; step <= 32'768; ++step) {
        const float modulation_octaves = static_cast<float>(step) / 4096.0f;
        const double expected_increment =
            reference_hz * std::exp2(static_cast<double>(modulation_octaves)) / sample_rate;
        const double actual_increment =
            reference_hz * static_cast<double>(FastMath::exp2(modulation_octaves)) / sample_rate;
        maximum_fm_phase_error =
            std::max(maximum_fm_phase_error, std::abs(actual_increment - expected_increment));
        all_fm_increments_are_positive_and_finite &=
            std::isfinite(actual_increment) && actual_increment > 0.0;
    }

    INFO("max pitch error = " << maximum_pitch_cents_error << " cents");
    INFO("max FM phase-increment error = " << maximum_fm_phase_error);
    REQUIRE(all_fm_increments_are_positive_and_finite);
    REQUIRE(maximum_pitch_cents_error < 0.001);
    REQUIRE(maximum_fm_phase_error < 1.0e-6);
}

TEST_CASE("FastMath log2 approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::log2(1.0f), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(FastMath::log2(2.0f), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(FastMath::log2(8.0f), WithinAbs(3.0, 0.02));
    REQUIRE_THAT(FastMath::log2(0.5f), WithinAbs(-1.0, 0.02));
}

TEST_CASE("FastMath log2 handles normalized power-of-two range", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::log2(0.25f), WithinAbs(-2.0f, 0.02f));
    REQUIRE_THAT(FastMath::log2(16.0f), WithinAbs(4.0f, 0.02f));
    REQUIRE_THAT(FastMath::log2(1024.0f), WithinAbs(10.0f, 0.02f));
}

TEST_CASE("FastMath log2 tracks mantissa-heavy values", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::log2(1.5f), WithinAbs(std::log2(1.5f), 0.02f));
    REQUIRE_THAT(FastMath::log2(3.25f), WithinAbs(std::log2(3.25f), 0.04f));
    REQUIRE_THAT(FastMath::log2(96.0f), WithinAbs(std::log2(96.0f), 0.03f));
}

TEST_CASE("FastMath pow approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::pow(2.0f, 3.0f), WithinAbs(8.0, 0.1));
    REQUIRE_THAT(FastMath::pow(10.0f, 2.0f), WithinAbs(100.0, 1.0));
    REQUIRE_THAT(FastMath::pow(0.5f, 2.0f), WithinAbs(0.25, 0.01));
}

TEST_CASE("FastMath pow and reciprocal guard edge inputs", "[signal][fast_math][issue-645]") {
    REQUIRE_THAT(FastMath::pow(0.0f, 2.0f), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(FastMath::pow(-2.0f, 3.0f), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(FastMath::pow(4.0f, 0.5f), WithinAbs(2.0f, 0.05f));

    REQUIRE_THAT(FastMath::rcp(4.0f), WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(FastMath::rcp(-2.0f), WithinAbs(-0.5f, 0.001f));
}

TEST_CASE("FastMath pow handles fractional and identity exponents", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::pow(9.0f, 0.5f), WithinAbs(3.0f, 0.08f));
    REQUIRE_THAT(FastMath::pow(7.0f, 0.0f), WithinAbs(1.0f, 0.02f));
    REQUIRE_THAT(FastMath::pow(1.0f, 32.0f), WithinAbs(1.0f, 0.02f));
}

TEST_CASE("FastMath db_to_gain", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::db_to_gain(0.0f), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(FastMath::db_to_gain(6.0f), WithinAbs(std::pow(10.0f, 6.0f / 20.0f), 0.05));
    REQUIRE_THAT(FastMath::db_to_gain(-6.0f), WithinAbs(std::pow(10.0f, -6.0f / 20.0f), 0.02));
    REQUIRE_THAT(FastMath::db_to_gain(-60.0f), WithinAbs(0.001, 0.001));
}

TEST_CASE("FastMath gain_to_db", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::gain_to_db(1.0f), WithinAbs(0.0, 0.1));
    REQUIRE_THAT(FastMath::gain_to_db(2.0f), WithinAbs(6.02, 0.1));
    REQUIRE_THAT(FastMath::gain_to_db(0.5f), WithinAbs(-6.02, 0.1));
    REQUIRE(FastMath::gain_to_db(0.0f) < -100.0f);
}

TEST_CASE("FastMath gain conversion handles negative silence floor",
          "[signal][fast_math][issue-645]") {
    REQUIRE_THAT(FastMath::gain_to_db(-1.0f), WithinAbs(-200.0f, 0.001f));
    REQUIRE(FastMath::db_to_gain(-120.0f) > 0.0f);
    REQUIRE(FastMath::db_to_gain(-120.0f) < 0.00001f);
}

TEST_CASE("FastMath rsqrt approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::rsqrt(1.0f), WithinAbs(1.0, 0.02));
    REQUIRE_THAT(FastMath::rsqrt(4.0f), WithinAbs(0.5, 0.02));
    REQUIRE_THAT(FastMath::rsqrt(0.25f), WithinAbs(2.0, 0.05));
}

TEST_CASE("FastMath soft_clip", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::soft_clip(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(FastMath::soft_clip(0.5f), WithinAbs(0.481, 0.01));
    REQUIRE_THAT(FastMath::soft_clip(1.5f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(FastMath::soft_clip(-1.5f), WithinAbs(-1.0, 0.001));
    REQUIRE_THAT(FastMath::soft_clip(2.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(FastMath::soft_clip(-2.0f), WithinAbs(-1.0, 0.001));
    // Symmetry
    REQUIRE_THAT(FastMath::soft_clip(0.3f), WithinAbs(-FastMath::soft_clip(-0.3f), 0.001));
}

TEST_CASE("FastMath clamp_unit", "[signal][fast_math]") {
    REQUIRE(FastMath::clamp_unit(1.0f) == 1.0f);
    REQUIRE(FastMath::clamp_unit(-1.0f) == -1.0f);
    REQUIRE(FastMath::clamp_unit(0.5f) == 0.5f);
    REQUIRE(FastMath::clamp_unit(2.0f) == 1.0f);
    REQUIRE(FastMath::clamp_unit(-2.0f) == -1.0f);
}

TEST_CASE("FastMath tanh is odd within unclamped range", "[signal][fast_math]") {
    for (float value : {0.125f, 0.75f, 2.5f, 3.75f}) {
        REQUIRE_THAT(FastMath::tanh(value), WithinAbs(-FastMath::tanh(-value), 1e-6f));
    }
}

TEST_CASE("FastMath exp2 and log2 round trip powers", "[signal][fast_math]") {
    for (float exponent : {-3.0f, -1.0f, 0.0f, 2.0f, 5.0f}) {
        const float value = FastMath::exp2(exponent);
        REQUIRE_THAT(FastMath::log2(value), WithinAbs(exponent, 0.02f));
    }
}

TEST_CASE("FastMath gain conversion round trips common levels", "[signal][fast_math]") {
    for (float db : {-24.0f, -12.0f, 0.0f, 12.0f}) {
        REQUIRE_THAT(FastMath::gain_to_db(FastMath::db_to_gain(db)), WithinAbs(db, 1.2f));
    }
}

TEST_CASE("FastMath soft clip is monotonic across control points", "[signal][fast_math]") {
    float previous = FastMath::soft_clip(-2.0f);
    for (float value : {-1.5f, -1.0f, -0.25f, 0.0f, 0.25f, 1.0f, 1.5f, 2.0f}) {
        const float clipped = FastMath::soft_clip(value);
        REQUIRE(clipped >= previous);
        previous = clipped;
    }
}

TEST_CASE("FastMath exp2 is monotonic across fractional octaves", "[signal][fast_math]") {
    float previous = FastMath::exp2(-2.0f);
    for (float exponent : {-1.5f, -0.25f, 0.0f, 0.5f, 1.25f, 2.0f}) {
        float next = FastMath::exp2(exponent);
        REQUIRE(next > previous);
        previous = next;
    }
}

TEST_CASE("FastMath log2 orders common gain ratios", "[signal][fast_math]") {
    REQUIRE(FastMath::log2(0.25f) < FastMath::log2(0.5f));
    REQUIRE(FastMath::log2(0.5f) < FastMath::log2(1.0f));
    REQUIRE(FastMath::log2(1.0f) < FastMath::log2(2.0f));
    REQUIRE(FastMath::log2(2.0f) < FastMath::log2(4.0f));
}

namespace {

// FastMath::tanh's contract written with early returns: exactly +/-1 beyond
// |x| = 4, the [7/6] Pade form inside.
float tanh_reference(float x) {
    if (x < -4.0f)
        return -1.0f;
    if (x > 4.0f)
        return 1.0f;
    const float x2 = x * x;
    const float num = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    const float den = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
    return num / den;
}

double ulp_of(double value) {
    const float f = static_cast<float>(std::abs(value));
    return static_cast<double>(std::nextafter(f, std::numeric_limits<float>::infinity()) - f);
}

} // namespace

TEST_CASE("FastMath::tanh is bit-identical to its saturating Pade contract",
          "[signal][fast-math][tanh]") {
    std::size_t mismatches = 0;
    for (float x = -12.0f; x <= 12.0f; x += 1.0e-4f) {
        const float a = FastMath::tanh(x);
        const float b = tanh_reference(x);
        mismatches += std::bit_cast<std::uint32_t>(a) != std::bit_cast<std::uint32_t>(b);
    }
    CHECK(mismatches == 0);
    CHECK(FastMath::tanh(4.0f) == tanh_reference(4.0f));
    CHECK(FastMath::tanh(-4.0f) == tanh_reference(-4.0f));
    CHECK(FastMath::tanh(std::nextafter(4.0f, 5.0f)) == 1.0f);
    CHECK(FastMath::tanh(std::nextafter(-4.0f, -5.0f)) == -1.0f);
    CHECK(FastMath::tanh(1.0e30f) == 1.0f);
    CHECK(FastMath::tanh(-1.0e30f) == -1.0f);
    CHECK(FastMath::tanh(std::numeric_limits<float>::infinity()) == 1.0f);
    CHECK(FastMath::tanh(-std::numeric_limits<float>::infinity()) == -1.0f);
    CHECK(std::isnan(FastMath::tanh(std::numeric_limits<float>::quiet_NaN())));
}

TEST_CASE("log10_float stays within a few ulps of libm", "[signal][fast-math][decibel]") {
    double worst_ulps = 0.0;
    for (float x = 1.0e-10f; x < 1.0e6f; x *= 1.0001f) {
        const double reference = std::log10(static_cast<double>(x));
        const double err =
            std::abs(static_cast<double>(pulp::signal::detail::log10_float(x)) - reference);
        // Near log10(x) = 0 the result ulp shrinks without bound; hold those
        // inputs to an absolute error instead.
        if (std::abs(reference) < 1.0e-3)
            CHECK(err < 1.0e-9);
        else
            worst_ulps = std::max(worst_ulps, err / ulp_of(reference));
    }
    CHECK(worst_ulps <= 4.0);
    CHECK(pulp::signal::detail::log10_float(1.0f) == 0.0f);
    CHECK(pulp::signal::detail::log10_float(1000.0f) == 3.0f);
    CHECK(pulp::signal::detail::log10_float(1.0e-10f) ==
          static_cast<float>(std::log10(static_cast<double>(1.0e-10f))));
}

TEST_CASE("exp10_float stays within a few parts in 1e7 of libm and saturates like pow",
          "[signal][fast-math][decibel]") {
    double worst = 0.0;
    for (float x = -37.0f; x < 38.0f; x += 1.0e-4f) {
        const double reference = std::pow(10.0, static_cast<double>(x));
        worst = std::max(
            worst, std::abs(static_cast<double>(pulp::signal::detail::exp10_float(x)) - reference) /
                       reference);
    }
    CHECK(worst < 2.0e-7);
    CHECK(pulp::signal::detail::exp10_float(0.0f) == 1.0f);
    CHECK(std::isinf(pulp::signal::detail::exp10_float(39.0f)));
    CHECK(std::isinf(pulp::signal::detail::exp10_float(1.0e30f)));
    CHECK(pulp::signal::detail::exp10_float(-46.0f) == 0.0f);
    CHECK(pulp::signal::detail::exp10_float(-1.0e30f) == 0.0f);
    CHECK_FALSE(std::signbit(pulp::signal::detail::exp10_float(-1.0e30f)));
    // Gradual underflow keeps the subnormal range, as std::pow does.
    const float sub = pulp::signal::detail::exp10_float(-40.0f);
    CHECK(sub > 0.0f);
    CHECK(std::abs(static_cast<double>(sub) - 1.0e-40) / 1.0e-40 < 1.0e-3);
}

TEST_CASE("Bit-operation selects match the conditionals they replace",
          "[signal][fast-math][select]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(pulp::signal::detail::select_bits(true, 1.5f, -2.0f) == 1.5f);
    CHECK(pulp::signal::detail::select_bits(false, 1.5f, -2.0f) == -2.0f);
    CHECK(std::isnan(pulp::signal::detail::select_bits(true, nan, 0.0f)));
    CHECK(std::signbit(pulp::signal::detail::select_bits(false, 0.0f, -0.0f)));
    CHECK(pulp::signal::detail::select_bits(true, 2.0, 3.0) == 2.0);
    std::size_t mismatches = 0;
    for (std::uint32_t bits = 0; bits < 0x7f800000u; bits += 4099) {
        for (const float sign : {1.0f, -1.0f}) {
            const float x = sign * std::bit_cast<float>(bits);
            mismatches +=
                std::bit_cast<std::uint32_t>(pulp::signal::detail::tanh_accurate_bits(x)) !=
                std::bit_cast<std::uint32_t>(FastMath::tanh_accurate(x));
            mismatches +=
                std::bit_cast<std::uint32_t>(pulp::signal::detail::snap_to_zero_bits(x)) !=
                std::bit_cast<std::uint32_t>(pulp::signal::snap_to_zero(x));
        }
    }
    CHECK(mismatches == 0);
    CHECK(std::isnan(pulp::signal::detail::tanh_accurate_bits(nan)));
    CHECK(std::isnan(pulp::signal::detail::snap_to_zero_bits(nan)));
}
