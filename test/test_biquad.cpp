#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/biquad.hpp>
#include "support/denormal_tail.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

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
          "[signal][biquad]") {
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
          "[signal][biquad]") {
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

TEST_CASE("Biquad64 lowpass impulse response is pinned",
          "[signal][biquad][f64]") {
    Biquad64 filter;
    filter.set_coefficients(Biquad64::Type::lowpass, 600.0, 0.707, 48000.0);

    std::array<double, 6> impulse{{1.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
    filter.process(impulse.data(), static_cast<int>(impulse.size()));

    const std::array<double, 6> expected{{
        0.0014603047124823571,
        0.0056791514333672331,
        0.010881557210226506,
        0.015473416853959747,
        0.019492101554219175,
        0.022974401307966137,
    }};

    for (std::size_t i = 0; i < impulse.size(); ++i) {
        REQUIRE(std::isfinite(impulse[i]));
        REQUIRE_THAT(impulse[i], WithinAbs(expected[i], 1e-14));
    }
}

TEST_CASE("Biquad64 sample and block processing paths match",
          "[signal][biquad][f64]") {
    Biquad64 sample_filter;
    Biquad64 block_filter;
    sample_filter.set_coefficients(Biquad64::Type::peaking, 1200.0, 0.75, 48000.0, 6.0);
    block_filter.set_coefficients(Biquad64::Type::peaking, 1200.0, 0.75, 48000.0, 6.0);

    const std::array<double, 12> input{{
        0.75, -0.5, 0.25, 0.0, 1.0, -1.0,
        0.125, -0.25, 0.5, -0.125, 0.0, 0.375,
    }};

    auto sample_output = input;
    for (double& sample : sample_output)
        sample = sample_filter.process(sample);

    auto block_output = input;
    block_filter.process(block_output.data(), static_cast<int>(block_output.size()));

    for (std::size_t i = 0; i < input.size(); ++i) {
        REQUIRE(std::isfinite(block_output[i]));
        REQUIRE_THAT(block_output[i], WithinAbs(sample_output[i], 1e-14));
    }
}

TEST_CASE("Biquad extreme valid controls still produce finite sample output",
          "[signal][biquad]") {
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

TEST_CASE("Biquad input guard leaves every finite positive control untouched",
          "[signal][biquad][coverage]") {
    // The guard substitutes values only for controls that would produce inf/NaN
    // (non-positive or non-finite q, sample rate, frequency). A tiny but positive
    // q is legitimate and must reach the design unchanged, so it has to produce
    // different coefficients from the guard's own substitute value.
    Biquad tiny_q, substitute_q;
    tiny_q.set_coefficients(Biquad::Type::peaking, 1200.0f, 1e-6f, kSampleRate, 6.0f);
    substitute_q.set_coefficients(Biquad::Type::peaking, 1200.0f, 1e-4f, kSampleRate, 6.0f);
    const auto a = tiny_q.coefficients();
    const auto b = substitute_q.coefficients();
    REQUIRE(std::isfinite(a.b0));
    REQUIRE(std::isfinite(a.a1));
    REQUIRE(std::isfinite(a.a2));
    REQUIRE((a.b0 != b.b0 || a.a2 != b.a2));

    // Control: an invalid q is replaced by that same substitute, so the two
    // filters must then agree exactly.
    Biquad zero_q;
    zero_q.set_coefficients(Biquad::Type::peaking, 1200.0f, 0.0f, kSampleRate, 6.0f);
    const auto z = zero_q.coefficients();
    REQUIRE(z.b0 == b.b0);
    REQUIRE(z.a2 == b.a2);
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

namespace {

// Continuously excited signal (two tones plus LCG noise) that keeps every
// recursive state far above the 1e-15 snap threshold, so the block and sample
// paths must agree to the bit.
std::vector<float> busy_signal(std::size_t n) {
    std::vector<float> s(n);
    std::uint32_t lcg = 0x2468aceu;
    for (std::size_t i = 0; i < n; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = static_cast<float>(lcg >> 8) / 16777216.0f - 0.5f;
        const float t = static_cast<float>(i) / kSampleRate;
        s[i] = 0.5f * std::sin(6.2831853f * 220.0f * t) +
               0.3f * std::sin(6.2831853f * 3150.0f * t) + 0.2f * noise;
    }
    return s;
}

// Uneven block lengths covering 1, chunk-boundary neighbours, and long blocks.
std::vector<int> split_schedule(std::size_t total) {
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

} // namespace

namespace {

double max_pole_radius(const BiquadCoefficients& c) {
    const double a1 = c.a1, a2 = c.a2, disc = a1 * a1 - 4.0 * a2;
    if (disc < 0.0)
        return std::sqrt(a2);
    const double root = std::sqrt(disc);
    return std::max(std::abs(-a1 + root), std::abs(-a1 - root)) * 0.5;
}

// RMS error of `actual` against a float64 DF2T running the same float
// coefficients: the exact filter the float paths approximate.
double rms_error_db(const BiquadCoefficients& c, const std::vector<float>& input,
                    const std::vector<float>& actual) {
    double s1 = 0.0, s2 = 0.0, acc = 0.0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double x = input[i];
        const double y = static_cast<double>(c.b0) * x + s1;
        s1 = static_cast<double>(c.b1) * x - static_cast<double>(c.a1) * y + s2;
        s2 = static_cast<double>(c.b2) * x - static_cast<double>(c.a2) * y;
        acc += (actual[i] - y) * (actual[i] - y);
    }
    return 10.0 * std::log10(acc / static_cast<double>(input.size()) + 1e-300);
}

std::vector<float> run_block(Biquad filter, const std::vector<float>& input,
                             const std::vector<int>& schedule) {
    auto out = input;
    std::size_t pos = 0;
    for (int block : schedule) {
        filter.process(out.data() + pos, block);
        pos += static_cast<std::size_t>(block);
    }
    return out;
}

} // namespace

TEST_CASE("Biquad block path error stays within 3 dB of the sample path's",
          "[signal][biquad][precision]") {
    // The float block path runs DF1 with double state for a shorter
    // loop-carried dependency, so it rounds differently from the sample path.
    // Its error against the exact filter must not be meaningfully larger.
    const auto input = busy_signal(16384);
    const std::vector<int> whole{static_cast<int>(input.size())};
    const Biquad::Type types[] = {Biquad::Type::lowpass,   Biquad::Type::highpass,
                                  Biquad::Type::bandpass,  Biquad::Type::notch,
                                  Biquad::Type::allpass,   Biquad::Type::peaking,
                                  Biquad::Type::low_shelf, Biquad::Type::high_shelf};
    for (const auto type : types) {
        for (const float freq : {20.0f, 100.0f, 1000.0f, 8000.0f}) {
            for (const float q : {0.7071f, 10.0f}) {
                INFO("type " << static_cast<int>(type) << " f " << freq << " q " << q);
                Biquad sample_filter;
                sample_filter.set_coefficients(type, freq, q, kSampleRate, 9.0f);
                const auto c = sample_filter.coefficients();
                auto by_sample = input;
                for (float& x : by_sample)
                    x = sample_filter.process(x);
                Biquad block_filter;
                block_filter.set_coefficients(c);
                const auto by_block = run_block(block_filter, input, whole);
                CHECK(rms_error_db(c, input, by_block) <= rms_error_db(c, input, by_sample) + 3.0);
            }
        }
    }
}

TEST_CASE("Biquad64 block path keeps the sample path's order near the unit circle",
          "[signal][biquad][determinism]") {
    // Double-precision filters keep a DF2T block path. Poles closer to the
    // unit circle than kFeedbackLastMaxPoleRadius make the regrouped order
    // measurably noisier, so those designs run the sample path's arithmetic
    // and stay bit-identical to it.
    const std::array<BiquadCase, 4> cases{{
        {Biquad::Type::low_shelf, 20.0f, 10.0f, 12.0f},
        {Biquad::Type::allpass, 20.0f, 0.7071f, 0.0f},
        {Biquad::Type::peaking, 100.0f, 10.0f, 12.0f},
        {Biquad::Type::highpass, 20.0f, 10.0f, 0.0f},
    }};
    const auto input_f = busy_signal(8192);
    const std::vector<double> input(input_f.begin(), input_f.end());
    for (const auto& c : cases) {
        INFO("type " << static_cast<int>(c.type) << " f " << c.freq_hz);
        Biquad64 sample_filter;
        sample_filter.set_coefficients(static_cast<Biquad64::Type>(c.type), c.freq_hz, c.q,
                                       static_cast<double>(kSampleRate), c.gain_db);
        const auto k = sample_filter.coefficients();
        REQUIRE(max_pole_radius(BiquadCoefficients{
                    static_cast<float>(k.b0), static_cast<float>(k.b1), static_cast<float>(k.b2),
                    static_cast<float>(k.a1), static_cast<float>(k.a2)}) >
                Biquad64::kFeedbackLastMaxPoleRadius);
        Biquad64 block_filter = sample_filter;
        auto expected = input;
        for (double& x : expected)
            x = sample_filter.process(x);
        auto actual = input;
        std::size_t pos = 0;
        for (int block : split_schedule(input.size())) {
            block_filter.process(actual.data() + pos, block);
            pos += static_cast<std::size_t>(block);
        }
        REQUIRE(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(double)) == 0);
    }
}

TEST_CASE("Biquad block output does not depend on how the buffer is split",
          "[signal][biquad][determinism]") {
    const auto input = busy_signal(8192);
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 1000.0f, 0.7071f, kSampleRate);
    REQUIRE(max_pole_radius(filter.coefficients()) <= Biquad::kFeedbackLastMaxPoleRadius);
    const auto whole = run_block(filter, input, {static_cast<int>(input.size())});
    const auto split = run_block(filter, input, split_schedule(input.size()));
    REQUIRE(std::memcmp(whole.data(), split.data(), whole.size() * sizeof(float)) == 0);
    // And it differs from the sample path only at round-off level.
    auto by_sample = input;
    Biquad sample_filter = filter;
    for (float& x : by_sample)
        x = sample_filter.process(x);
    float worst = 0.0f;
    for (std::size_t i = 0; i < input.size(); ++i)
        worst = std::max(worst, std::abs(whole[i] - by_sample[i]));
    CHECK(worst < 1.0e-5f);
}

TEST_CASE("Biquad block path flushes a decaying tail without subnormal residency",
          "[signal][biquad][denormal]") {
    using namespace pulp::test::denormal_tail;
    if (flush_to_zero_active())
        SKIP("this thread flushes subnormals to zero; the tail counts would be vacuous");

    constexpr std::size_t kTotal = std::size_t{1} << 18;

    // Negative control: the same coefficients in a DF2T with no snap. A 20 Hz
    // lowpass has its poles close enough to |z| = 1 that the float tail rounds
    // into a subnormal limit cycle and never reaches zero. If this stops
    // happening the stimulus no longer exercises the hazard.
    Biquad slow;
    slow.set_coefficients(Biquad::Type::lowpass, 20.0f, 0.7071f, kSampleRate);
    const auto k = slow.coefficients();
    float s1 = 0.0f;
    float s2 = 0.0f;
    const auto control = impulse_tail(
        [&](float* x, int n) {
            for (int i = 0; i < n; ++i) {
                const float out = k.b0 * x[i] + s1;
                s1 = k.b1 * x[i] - k.a1 * out + s2;
                s2 = k.b2 * x[i] - k.a2 * out;
                x[i] = out;
            }
        },
        kTotal, 512);
    REQUIRE(control.subnormal_outputs > kTotal / 2);
    REQUIRE(control.nonzero_end == kTotal);

    for (std::size_t block :
         {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{4096}, kTotal}) {
        INFO("block " << block);
        Biquad filter;
        filter.set_coefficients(Biquad::Type::lowpass, 20.0f, 0.7071f, kSampleRate);
        const auto tail =
            impulse_tail([&](float* x, int n) { filter.process(x, n); }, kTotal, block);
        REQUIRE(tail.subnormal_outputs == 0);
        REQUIRE(tail.nonzero_end > 0);
        // A one-sample block snaps every sample, as the sample path does; the
        // chunked snap must still bring the tail to exact zero.
        if (block > 1)
            REQUIRE(tail.nonzero_end < kTotal / 8);
    }

    // Poles at |z| ~= 0.41: the one regime where a state can fall from above
    // the snap threshold into the subnormal range inside a single chunk. The
    // tail must still reach exact zero, with at most one chunk of subnormal
    // output.
    for (std::size_t block : {std::size_t{64}, kTotal}) {
        INFO("block " << block);
        Biquad filter;
        filter.set_coefficients(Biquad::Type::lowpass, kSampleRate / 4.0f, 0.7071f, kSampleRate);
        const auto tail =
            impulse_tail([&](float* x, int n) { filter.process(x, n); }, kTotal, block);
        REQUIRE(tail.subnormal_outputs <= pulp::signal::kSnapChunkSamples);
        REQUIRE(tail.nonzero_end < 1024);
    }
}

TEST_CASE("Biquad stereo path matches two block calls bit for bit",
          "[signal][biquad][determinism]") {
    const auto left_in = busy_signal(8192);
    auto right_in = busy_signal(8192);
    for (float& x : right_in)
        x *= -0.7f;
    // Same-form pairs run in vector lanes; a mixed pair (one design near the
    // unit circle) falls back to two calls. Both must match exactly.
    const std::array<std::pair<BiquadCase, BiquadCase>, 3> pairs{{
        {{Biquad::Type::lowpass, 1000.0f, 0.7071f, 0.0f},
         {Biquad::Type::lowpass, 1000.0f, 0.7071f, 0.0f}},
        {{Biquad::Type::peaking, 3000.0f, 1.0f, 6.0f},
         {Biquad::Type::highpass, 200.0f, 0.7071f, 0.0f}},
        {{Biquad::Type::lowpass, 1000.0f, 0.7071f, 0.0f},
         {Biquad::Type::low_shelf, 20.0f, 10.0f, 12.0f}},
    }};
    for (const auto& [lc, rc] : pairs) {
        Biquad l_mono, r_mono;
        l_mono.set_coefficients(lc.type, lc.freq_hz, lc.q, kSampleRate, lc.gain_db);
        r_mono.set_coefficients(rc.type, rc.freq_hz, rc.q, kSampleRate, rc.gain_db);
        Biquad l_pair = l_mono, r_pair = r_mono;
        auto l_expected = left_in, r_expected = right_in, l_actual = left_in, r_actual = right_in;
        std::size_t pos = 0;
        for (int block : split_schedule(left_in.size())) {
            l_mono.process(l_expected.data() + pos, block);
            r_mono.process(r_expected.data() + pos, block);
            Biquad::process_stereo(l_pair, r_pair, l_actual.data() + pos, r_actual.data() + pos,
                                   block);
            pos += static_cast<std::size_t>(block);
        }
        CHECK(std::memcmp(l_actual.data(), l_expected.data(), l_actual.size() * sizeof(float)) ==
              0);
        CHECK(std::memcmp(r_actual.data(), r_expected.data(), r_actual.size() * sizeof(float)) ==
              0);
    }
}

TEST_CASE("Biquad::process_chain matches chained sample paths for any section count",
          "[signal][biquad][determinism]") {
    const auto input = busy_signal(4096);
    // Passes longer than kWideStateMaxSections that contain a section near
    // the unit circle (the 31.25 Hz band) run the sample path's order: below,
    // at and above one pass, with a remainder pass of five.
    for (std::size_t count : {std::size_t{5}, std::size_t{16}, std::size_t{21}, std::size_t{31}}) {
        INFO("sections " << count);
        std::vector<Biquad> by_sample(count);
        for (std::size_t k = 0; k < count; ++k)
            by_sample[k].set_coefficients(Biquad::Type::peaking,
                                          31.25f * std::pow(2.0f, static_cast<float>(k % 10)), 1.4f,
                                          kSampleRate, k % 2 ? -4.0f : 5.0f);
        auto chained = by_sample;
        auto expected = input;
        for (float& x : expected)
            for (auto& section : by_sample)
                x = section.process(x);
        auto actual = input;
        std::size_t pos = 0;
        for (int block : split_schedule(actual.size())) {
            Biquad::process_chain(chained.data(), count, actual.data() + pos,
                                  static_cast<std::size_t>(block));
            pos += static_cast<std::size_t>(block);
        }
        CHECK(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(float)) == 0);
    }
}

TEST_CASE("Biquad block path snaps a subnormal state within 64 samples",
          "[signal][biquad][denormal]") {
    using namespace pulp::test::denormal_tail;
    if (flush_to_zero_active())
        SKIP("this thread flushes subnormals to zero; the tail counts would be vacuous");
    // A subnormal impulse puts the state straight into the subnormal range,
    // where a 20 Hz lowpass rounds into a cycle that never decays on its own.
    // Only the snap clears it, so the subnormal run length is the snap
    // interval: this pins the documented 64 samples, not just "one chunk".
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 20.0f, 0.7071f, kSampleRate);
    std::vector<float> buffer(8192, 0.0f);
    buffer[0] = 1.0e-39f;
    REQUIRE(pulp::signal::is_denormal(buffer[0]));
    filter.process(buffer.data(), static_cast<int>(buffer.size()));
    std::size_t subnormal = 0;
    std::size_t nonzero_end = 0;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        subnormal += pulp::signal::is_denormal(buffer[i]) ? 1u : 0u;
        if (buffer[i] != 0.0f)
            nonzero_end = i + 1;
    }
    CHECK(subnormal > 0);
    CHECK(nonzero_end <= 64);
}

TEST_CASE("Biquad block and stereo paths under per-block and per-32-sample cutoff sweeps",
          "[signal][biquad][modulation]") {
    // Coefficients change at a block boundary on every path. The block form
    // can switch at the pole-radius threshold mid-stream; both forms keep the
    // same DF2T state, so the sweep must stay within the static tolerance of
    // the sample path and stereo must stay bit-identical to mono.
    const auto input = busy_signal(16384);
    for (const int step : {32, 256}) {
        INFO("retune every " << step << " samples");
        Biquad by_sample, by_block, left, right, right_mono;
        std::vector<float> a = input, b = input, l = input, r = input, rm = input;
        for (float& x : r)
            x *= -0.5f;
        rm = r;
        double s1 = 0.0, s2 = 0.0, err_sample = 0.0, err_block = 0.0;
        for (std::size_t pos = 0; pos < input.size(); pos += static_cast<std::size_t>(step)) {
            // Sweep 30 Hz .. 12 kHz and back: crosses the 0.995 radius switch.
            const float phase = static_cast<float>(pos) / static_cast<float>(input.size());
            const float fc =
                30.0f * std::pow(400.0f, 0.5f - 0.5f * std::cos(6.2831853f * 2.0f * phase));
            for (Biquad* f : {&by_sample, &by_block, &left, &right, &right_mono})
                f->set_coefficients(Biquad::Type::lowpass, fc, 0.9f, kSampleRate);
            const auto c = by_sample.coefficients();
            const std::size_t n = static_cast<std::size_t>(step);
            for (std::size_t i = pos; i < pos + n; ++i) {
                a[i] = by_sample.process(a[i]);
                const double x = input[i];
                const double y = static_cast<double>(c.b0) * x + s1;
                s1 = static_cast<double>(c.b1) * x - static_cast<double>(c.a1) * y + s2;
                s2 = static_cast<double>(c.b2) * x - static_cast<double>(c.a2) * y;
                err_sample += (a[i] - y) * (a[i] - y);
            }
            by_block.process(b.data() + pos, step);
            Biquad::process_stereo(left, right, l.data() + pos, r.data() + pos, step);
            right_mono.process(rm.data() + pos, step);
        }
        // The block path's error against the exact filter on the same schedule.
        Biquad replay;
        double t1 = 0.0, t2 = 0.0;
        for (std::size_t pos = 0; pos < input.size(); pos += static_cast<std::size_t>(step)) {
            const float phase = static_cast<float>(pos) / static_cast<float>(input.size());
            const float fc =
                30.0f * std::pow(400.0f, 0.5f - 0.5f * std::cos(6.2831853f * 2.0f * phase));
            replay.set_coefficients(Biquad::Type::lowpass, fc, 0.9f, kSampleRate);
            const auto c = replay.coefficients();
            for (std::size_t i = pos; i < pos + static_cast<std::size_t>(step); ++i) {
                const double x = input[i];
                const double y = static_cast<double>(c.b0) * x + t1;
                t1 = static_cast<double>(c.b1) * x - static_cast<double>(c.a1) * y + t2;
                t2 = static_cast<double>(c.b2) * x - static_cast<double>(c.a2) * y;
                err_block += (b[i] - y) * (b[i] - y);
            }
        }
        CHECK(10.0 * std::log10(err_block + 1e-300) <=
              10.0 * std::log10(err_sample + 1e-300) + 3.0);
        CHECK(std::memcmp(l.data(), b.data(), l.size() * sizeof(float)) == 0);
        CHECK(std::memcmp(r.data(), rm.data(), r.size() * sizeof(float)) == 0);
    }
}

TEST_CASE("Biquad::process_chain short passes stay within the sample path's accuracy",
          "[signal][biquad][precision]") {
    // Passes of up to kWideStateMaxSections float sections run DF1 with the
    // state and the signal between sections in double. Against a float64
    // cascade their RMS error must not exceed the chained sample paths' by
    // more than 3 dB, and in practice is far below it.
    const auto input = busy_signal(16384);
    for (std::size_t count = 1; count <= Biquad::kWideStateMaxSections; ++count) {
        INFO("sections " << count);
        std::vector<Biquad> by_sample(count);
        for (std::size_t k = 0; k < count; ++k)
            by_sample[k].set_coefficients(Biquad::Type::peaking,
                                          40.0f * std::pow(3.0f, static_cast<float>(k)), 2.0f,
                                          kSampleRate, k % 2 ? -6.0f : 6.0f);
        auto chained = by_sample;
        std::vector<BiquadCoefficients> c(count);
        for (std::size_t k = 0; k < count; ++k)
            c[k] = by_sample[k].coefficients();
        auto expected = input;
        for (float& x : expected)
            for (auto& section : by_sample)
                x = section.process(x);
        auto actual = input;
        std::size_t pos = 0;
        for (int block : split_schedule(actual.size())) {
            Biquad::process_chain(chained.data(), count, actual.data() + pos,
                                  static_cast<std::size_t>(block));
            pos += static_cast<std::size_t>(block);
        }
        std::vector<double> s1(count, 0.0), s2(count, 0.0);
        double err_sample = 0.0, err_block = 0.0;
        for (std::size_t i = 0; i < input.size(); ++i) {
            double v = input[i];
            for (std::size_t k = 0; k < count; ++k) {
                const double y = static_cast<double>(c[k].b0) * v + s1[k];
                s1[k] = static_cast<double>(c[k].b1) * v - static_cast<double>(c[k].a1) * y + s2[k];
                s2[k] = static_cast<double>(c[k].b2) * v - static_cast<double>(c[k].a2) * y;
                v = y;
            }
            err_sample += (expected[i] - v) * (expected[i] - v);
            err_block += (actual[i] - v) * (actual[i] - v);
        }
        CHECK(10.0 * std::log10(err_block + 1e-300) <=
              10.0 * std::log10(err_sample + 1e-300) + 3.0);
    }
}

TEST_CASE("Biquad block path continues the sample path's state exactly in real arithmetic",
          "[signal][biquad][determinism]") {
    // Mixing per-sample and block calls on one filter (and retuning between
    // them) hands the DF2T state to the DF1 block path and back. The result
    // must track an all-sample-path run to round-off.
    const auto input = busy_signal(6000);
    Biquad mixed, reference;
    mixed.set_coefficients(Biquad::Type::lowpass, 900.0f, 1.2f, kSampleRate);
    reference = mixed;
    auto a = input;
    auto b = input;
    float worst = 0.0f;
    for (std::size_t pos = 0; pos < input.size(); pos += 500) {
        if (pos == 2500) {
            mixed.set_coefficients(Biquad::Type::lowpass, 3000.0f, 0.7f, kSampleRate);
            reference.set_coefficients(Biquad::Type::lowpass, 3000.0f, 0.7f, kSampleRate);
        }
        if ((pos / 500) % 2 == 0)
            mixed.process(a.data() + pos, 500);
        else
            for (std::size_t i = pos; i < pos + 500; ++i)
                a[i] = mixed.process(a[i]);
        for (std::size_t i = pos; i < pos + 500; ++i)
            b[i] = reference.process(b[i]);
    }
    for (std::size_t i = 0; i < input.size(); ++i)
        worst = std::max(worst, std::abs(a[i] - b[i]));
    CHECK(worst < 1.0e-5f);
}
