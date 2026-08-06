#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/filter_design.hpp>
#include <pulp/signal/iir_design.hpp>
#include <pulp/signal/sos_cascade.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <limits>
#include <span>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

template <typename T> struct Df1State {
    long double x1{}, x2{}, y1{}, y2{};
};

template <typename T>
std::vector<long double> oracle_impulse(std::span<const BiquadCoefficientsT<T>> sos,
                                        std::size_t count) {
    std::vector<Df1State<T>> states(sos.size());
    std::vector<long double> result(count);
    for (std::size_t n = 0; n < count; ++n) {
        long double value = n == 0 ? 1.0L : 0.0L;
        for (std::size_t i = 0; i < sos.size(); ++i) {
            const auto& c = sos[i];
            auto& s = states[i];
            const long double output =
                static_cast<long double>(c.b0) * value + static_cast<long double>(c.b1) * s.x1 +
                static_cast<long double>(c.b2) * s.x2 - static_cast<long double>(c.a1) * s.y1 -
                static_cast<long double>(c.a2) * s.y2;
            s.x2 = s.x1;
            s.x1 = value;
            s.y2 = s.y1;
            s.y1 = output;
            value = output;
        }
        result[n] = value;
    }
    return result;
}

template <typename T>
long double oracle_magnitude(std::span<const BiquadCoefficientsT<T>> sos, long double frequency,
                             long double sample_rate) {
    const long double pi = std::acos(-1.0L);
    const auto z1 = std::polar(1.0L, -2.0L * pi * frequency / sample_rate);
    const auto z2 = z1 * z1;
    std::complex<long double> response{1.0L, 0.0L};
    for (const auto& c : sos) {
        response *=
            (static_cast<long double>(c.b0) + static_cast<long double>(c.b1) * z1 +
             static_cast<long double>(c.b2) * z2) /
            (1.0L + static_cast<long double>(c.a1) * z1 + static_cast<long double>(c.a2) * z2);
    }
    return std::abs(response);
}

template <typename Sample, typename Coeff>
void require_impulse_matches(std::span<const BiquadCoefficientsT<Coeff>> sos,
                             long double tolerance) {
    SosCascadeT<Sample, 8> cascade;
    REQUIRE(cascade.prepare(8));
    REQUIRE(cascade.set_coefficients(sos) == SosCascadeInstallStatus::installed);
    const auto expected = oracle_impulse(sos, 512);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const Sample input = i == 0 ? Sample{1} : Sample{0};
        REQUIRE(std::abs(static_cast<long double>(cascade.process(input)) - expected[i]) <=
                tolerance);
    }
}

template <typename Sample, typename Coeff>
void require_frequency_matches(std::span<const BiquadCoefficientsT<Coeff>> sos,
                               long double tolerance) {
    constexpr long double sample_rate = 48000.0L;
    // Integer-bin frequencies over the measured window remove projection
    // leakage from this executor-vs-polynomial comparison.
    for (long double frequency : {187.5L, 750.0L, 2250.0L, 6000.0L, 15000.0L}) {
        SosCascadeT<Sample, 8> cascade;
        REQUIRE(cascade.prepare(8));
        REQUIRE(cascade.set_coefficients(sos) == SosCascadeInstallStatus::installed);
        const long double omega = 2.0L * std::acos(-1.0L) * frequency / sample_rate;
        long double sin_sum = 0.0L;
        long double cos_sum = 0.0L;
        constexpr int warmup = 8192;
        constexpr int measured = 8192;
        for (int n = 0; n < warmup + measured; ++n) {
            const long double phase = omega * static_cast<long double>(n);
            const auto output = cascade.process(static_cast<Sample>(std::sin(phase)));
            if (n >= warmup) {
                sin_sum += static_cast<long double>(output) * std::sin(phase);
                cos_sum += static_cast<long double>(output) * std::cos(phase);
            }
        }
        const long double measured_gain = 2.0L * std::hypot(sin_sum, cos_sum) / measured;
        REQUIRE(std::abs(measured_gain - oracle_magnitude(sos, frequency, sample_rate)) <=
                tolerance);
    }
}

template <typename Sample> void require_design_families() {
    auto butterworth = FilterDesign::butterworth_lowpass(8, 2200.0f, 48000.0f);
    auto chebyshev = IirDesign::chebyshev1_lowpass(8, 2200.0f, 0.75f, 48000.0f);
    auto elliptic = IirDesign::elliptic_lowpass(8, 2200.0f, 0.5f, 60.0f, 48000.0f);
    const long double impulse_tolerance = std::is_same_v<Sample, float> ? 4e-6L : 2e-13L;
    const long double frequency_tolerance = std::is_same_v<Sample, float> ? 2e-3L : 1.5e-3L;
    require_impulse_matches<Sample>(std::span<const BiquadCoefficients>{butterworth},
                                    impulse_tolerance);
    require_impulse_matches<Sample>(std::span<const BiquadCoefficients>{chebyshev},
                                    impulse_tolerance);
    require_impulse_matches<Sample>(std::span<const BiquadCoefficients>{elliptic},
                                    impulse_tolerance);
    require_frequency_matches<Sample>(std::span<const BiquadCoefficients>{butterworth},
                                      frequency_tolerance);
    require_frequency_matches<Sample>(std::span<const BiquadCoefficients>{chebyshev},
                                      frequency_tolerance);
    require_frequency_matches<Sample>(std::span<const BiquadCoefficients>{elliptic},
                                      frequency_tolerance);
}

} // namespace

TEST_CASE("SOS cascade matches independent DF1 and complex-response oracles",
          "[signal][sos-cascade][oracle]") {
    SECTION("float") {
        require_design_families<float>();
    }
    SECTION("double") {
        require_design_families<double>();
    }
}

TEST_CASE("SOS cascade installation is whole-cascade transactional",
          "[signal][sos-cascade][contract]") {
    SosCascadeT<float, 4> cascade, unchanged;
    REQUIRE(cascade.prepare(2));
    REQUIRE(unchanged.prepare(2));
    const auto valid = FilterDesign::butterworth_lowpass(4, 2000.0f, 48000.0f);
    REQUIRE(cascade.set_coefficients(std::span{valid}) == SosCascadeInstallStatus::installed);
    REQUIRE(unchanged.set_coefficients(std::span{valid}) == SosCascadeInstallStatus::installed);
    const auto before = cascade.coefficients(0);
    static_cast<void>(cascade.process(1.0f));
    static_cast<void>(unchanged.process(1.0f));

    REQUIRE_FALSE(cascade.prepare(0));
    REQUIRE(cascade.process(0.0f) == unchanged.process(0.0f));
    REQUIRE_FALSE(cascade.prepare(5));
    REQUIRE(cascade.process(0.0f) == unchanged.process(0.0f));

    auto bad = valid;
    bad[1].a2 = 1.0f;
    REQUIRE(cascade.set_coefficients(std::span{bad}) == SosCascadeInstallStatus::unstable);
    REQUIRE(cascade.coefficients(0).b0 == before.b0);
    REQUIRE(cascade.process(0.0f) == unchanged.process(0.0f));

    bad = valid;
    bad[1].b0 = std::numeric_limits<float>::infinity();
    REQUIRE(cascade.set_coefficients(std::span{bad}) == SosCascadeInstallStatus::non_finite);
    REQUIRE(cascade.coefficients(0).b0 == before.b0);
    REQUIRE(cascade.process(0.0f) == unchanged.process(0.0f));

    std::array<BiquadCoefficients, 3> too_many{};
    REQUIRE(cascade.set_coefficients(std::span{too_many}) ==
            SosCascadeInstallStatus::over_capacity);
    REQUIRE(cascade.size() == 2);
    REQUIRE(cascade.process(0.0f) == unchanged.process(0.0f));

    std::array<BiquadCoefficientsT<double>, 2> overflow{};
    overflow[1].b0 = std::numeric_limits<double>::max();
    REQUIRE(cascade.set_coefficients(std::span{overflow}) == SosCascadeInstallStatus::non_finite);
    REQUIRE(cascade.process(0.0f) == unchanged.process(0.0f));

    for (int i = 0; i < 64; ++i)
        REQUIRE(cascade.process(0.0f) == unchanged.process(0.0f));
}

TEST_CASE("SOS cascade installs coefficient precision independently of sample precision",
          "[signal][sos-cascade][precision]") {
    const auto float_design = IirDesign::chebyshev1_lowpass(6, 2400.0f, 0.5f, 48000.0f);
    std::vector<BiquadCoefficientsT<double>> double_design;
    for (const auto& c : float_design)
        double_design.push_back({c.b0, c.b1, c.b2, c.a1, c.a2});

    SosCascadeT<float, 4> float_executor;
    SosCascadeT<double, 4> double_executor;
    REQUIRE(float_executor.prepare(4));
    REQUIRE(double_executor.prepare(4));
    REQUIRE(float_executor.set_coefficients(std::span{double_design}) ==
            SosCascadeInstallStatus::installed);
    REQUIRE(double_executor.set_coefficients(std::span{float_design}) ==
            SosCascadeInstallStatus::installed);
    for (int i = 0; i < 512; ++i) {
        const float input = i == 0 ? 1.0f : 0.0f;
        REQUIRE_THAT(float_executor.process(input),
                     WithinAbs(static_cast<float>(double_executor.process(input)), 4e-6f));
    }
}

TEST_CASE("SOS cascade prepared capacity and bypass are explicit",
          "[signal][sos-cascade][capacity]") {
    SosCascadeT<double, 4> cascade;
    std::array<BiquadCoefficients, 1> identity{};
    REQUIRE(cascade.set_coefficients(std::span{identity}) == SosCascadeInstallStatus::not_prepared);
    REQUIRE_FALSE(cascade.prepare(0));
    REQUIRE_FALSE(cascade.prepare(5));
    REQUIRE(cascade.prepare(3));
    REQUIRE(cascade.capacity() == 3);
    REQUIRE(cascade.set_coefficients(std::span<const BiquadCoefficients>{}) ==
            SosCascadeInstallStatus::installed);
    REQUIRE(cascade.process(0.375) == 0.375);
}

TEST_CASE("SOS cascade replacement policy controls recursive tails",
          "[signal][sos-cascade][transition]") {
    const auto first = FilterDesign::butterworth_lowpass(4, 1200.0f, 48000.0f);
    const auto second = FilterDesign::butterworth_lowpass(4, 5000.0f, 48000.0f);
    SosCascadeT<float, 4> preserve, reset, fresh;
    for (auto* cascade : {&preserve, &reset, &fresh})
        REQUIRE(cascade->prepare(4));
    REQUIRE(preserve.set_coefficients(std::span{first}) == SosCascadeInstallStatus::installed);
    REQUIRE(reset.set_coefficients(std::span{first}) == SosCascadeInstallStatus::installed);
    for (int i = 0; i < 32; ++i) {
        static_cast<void>(preserve.process(i == 0 ? 1.0f : 0.0f));
        static_cast<void>(reset.process(i == 0 ? 1.0f : 0.0f));
    }
    REQUIRE(preserve.set_coefficients(std::span{second}, SosCascadeTransition::preserve_state) ==
            SosCascadeInstallStatus::installed);
    REQUIRE(reset.set_coefficients(std::span{second}, SosCascadeTransition::reset_state) ==
            SosCascadeInstallStatus::installed);
    REQUIRE(fresh.set_coefficients(std::span{second}) == SosCascadeInstallStatus::installed);
    REQUIRE(std::abs(preserve.process(0.0f)) > 1e-7f);
    REQUIRE(reset.process(0.0f) == fresh.process(0.0f));
    reset.reset();
    fresh.reset();
    REQUIRE(reset.process(0.25f) == fresh.process(0.25f));

    SosCascadeT<float, 4> resized;
    Biquad first_reference, second_reference;
    REQUIRE(resized.prepare(4));
    REQUIRE(resized.set_coefficients(std::span{first}) == SosCascadeInstallStatus::installed);
    first_reference.set_coefficients(first[0]);
    second_reference.set_coefficients(first[1]);
    for (int i = 0; i < 16; ++i) {
        const float input = i == 0 ? 1.0f : 0.0f;
        static_cast<void>(resized.process(input));
        static_cast<void>(second_reference.process(first_reference.process(input)));
    }
    REQUIRE(
        resized.set_coefficients(std::span{first}.first(1), SosCascadeTransition::preserve_state) ==
        SosCascadeInstallStatus::installed);
    REQUIRE_THAT(resized.process(0.0f), WithinAbs(first_reference.process(0.0f), 1e-7f));
    REQUIRE(resized.set_coefficients(std::span{first}, SosCascadeTransition::preserve_state) ==
            SosCascadeInstallStatus::installed);
    second_reference.reset();
    REQUIRE_THAT(resized.process(0.0f),
                 WithinAbs(second_reference.process(first_reference.process(0.0f)), 1e-7f));
}

TEST_CASE("SOS cascade preserves caller ordering and bounded headroom",
          "[signal][sos-cascade][ordering]") {
    auto sections = IirDesign::elliptic_lowpass(8, 3000.0f, 0.5f, 70.0f, 48000.0f);
    auto reversed = sections;
    std::reverse(reversed.begin(), reversed.end());
    SosCascadeT<double, 8> forward, reverse;
    REQUIRE(forward.prepare(8));
    REQUIRE(reverse.prepare(8));
    REQUIRE(forward.set_coefficients(std::span{sections}) == SosCascadeInstallStatus::installed);
    REQUIRE(reverse.set_coefficients(std::span{reversed}) == SosCascadeInstallStatus::installed);
    REQUIRE(forward.coefficients(0).b0 == static_cast<double>(sections[0].b0));
    double peak_forward = 0.0, peak_reverse = 0.0;
    for (int n = 0; n < 8192; ++n) {
        const double input = 16.0 * std::sin(0.071 * n);
        const double a = forward.process(input), b = reverse.process(input);
        REQUIRE(std::isfinite(a));
        REQUIRE(std::isfinite(b));
        peak_forward = std::max(peak_forward, std::abs(a));
        peak_reverse = std::max(peak_reverse, std::abs(b));
        REQUIRE_THAT(a, WithinAbs(b, 2e-8));
    }
    REQUIRE(peak_forward < 32.0);
    REQUIRE(peak_reverse < 32.0);
}

TEST_CASE("SOS cascade process install and reset allocate no memory", "[signal][sos-cascade][rt]") {
    SosCascadeT<float, 8> cascade;
    auto sections = IirDesign::chebyshev2_lowpass(8, 5000.0f, 60.0f, 48000.0f);
    bool prepared = false;
    SosCascadeInstallStatus installed = SosCascadeInstallStatus::not_prepared;
    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe probe;
        prepared = cascade.prepare(8);
        installed = cascade.set_coefficients(std::span{sections});
        for (int i = 0; i < 4096; ++i)
            static_cast<void>(cascade.process(i == 0 ? 1.0f : 0.0f));
        cascade.reset();
        allocation_count = probe.allocation_count();
    }
    REQUIRE(prepared);
    REQUIRE(installed == SosCascadeInstallStatus::installed);
    REQUIRE(allocation_count == 0);
}

TEST_CASE("SOS cascade recursive tails converge to exact zero", "[signal][sos-cascade][denormal]") {
    SosCascadeT<float, 4> cascade;
    auto sections = FilterDesign::butterworth_lowpass(4, 1000.0f, 48000.0f);
    REQUIRE(cascade.prepare(4));
    REQUIRE(cascade.set_coefficients(std::span{sections}) == SosCascadeInstallStatus::installed);
    static_cast<void>(cascade.process(1.0f));
    float output = 1.0f;
    bool saw_subnormal = false;
    for (int i = 0; i < 20000; ++i) {
        output = cascade.process(0.0f);
        saw_subnormal = saw_subnormal || std::fpclassify(output) == FP_SUBNORMAL;
    }
    REQUIRE_FALSE(saw_subnormal);
    // A downstream section can emit the upstream section's last pre-snap
    // normal value; the complete tail remains below four sections' snap floor.
    REQUIRE(std::abs(output) <= 4e-15f);
}

TEST_CASE("SOS cascade Release throughput stays bounded", "[signal][sos-cascade][benchmark]") {
    SosCascadeT<float, 16> cascade;
    auto sections = FilterDesign::butterworth_lowpass(32, 3000.0f, 48000.0f);
    REQUIRE(cascade.prepare(16));
    REQUIRE(sections.size() == cascade.capacity());
    REQUIRE(cascade.set_coefficients(std::span{sections}) == SosCascadeInstallStatus::installed);
    volatile float sink = 0.0f;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1'000'000; ++i)
        sink = cascade.process(0.125f + sink * 0.0f);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(std::isfinite(sink));
#ifdef NDEBUG
    REQUIRE(elapsed < std::chrono::seconds(2));
#else
    static_cast<void>(elapsed);
#endif
}
