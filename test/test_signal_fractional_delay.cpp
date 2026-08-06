#include <pulp/signal/fractional_delay.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cfenv>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::FractionalDelayLineT;
using pulp::signal::FractionalDelayMethod;
using pulp::signal::FractionalDelayStatus;

namespace {

class RoundingModeGuard {
  public:
    RoundingModeGuard() : original_(std::fegetround()) {}
    ~RoundingModeGuard() {
        if (original_ != -1)
            std::fesetround(original_);
    }

    RoundingModeGuard(const RoundingModeGuard&) = delete;
    RoundingModeGuard& operator=(const RoundingModeGuard&) = delete;

  private:
    int original_;
};

std::complex<double> independent_thiran_response(double delay, double omega) {
    const auto a = (1.0 - delay) / (1.0 + delay);
    const auto z1 = std::exp(std::complex<double>{0.0, -omega});
    return (a + z1) / (1.0 + a * z1);
}

double independent_group_delay(double delay, double omega) {
    constexpr double step = 1.0e-6;
    const auto ratio = independent_thiran_response(delay, omega + step) /
                       independent_thiran_response(delay, omega - step);
    return -std::arg(ratio) / (2.0 * step);
}

template <typename SampleType>
std::vector<SampleType> render(FractionalDelayMethod method, const std::vector<SampleType>& input,
                               const std::vector<double>& delays, std::size_t maximum_delay = 32) {
    FractionalDelayLineT<SampleType> line;
    REQUIRE(line.prepare(maximum_delay, method));
    std::vector<SampleType> output(input.size());
    const auto result = line.process(input.data(), output.data(), delays.data(), input.size());
    REQUIRE(result);
    return output;
}

double fitted_complex_error(const std::vector<double>& output, double omega, double delay,
                            std::size_t begin) {
    std::complex<long double> measured{};
    for (std::size_t n = begin; n < output.size(); ++n) {
        const auto phase = std::exp(std::complex<long double>{
            0.0L, -static_cast<long double>(omega) * static_cast<long double>(n)});
        measured += static_cast<long double>(output[n]) * phase;
    }
    measured *= 2.0L / static_cast<long double>(output.size() - begin);
    const auto ideal =
        std::exp(std::complex<long double>{0.0L, -static_cast<long double>(omega * delay)});
    return static_cast<double>(std::abs(measured - ideal));
}

} // namespace

TEST_CASE("Thiran-1 is stable, allpass, and matches an independent group-delay oracle",
          "[signal][fractional-delay]") {
    constexpr std::array delays{1.0, 1.000001, 1.25, 1.75, 1.999999, 2.0};
    constexpr std::array omegas{0.0, 0.07, 0.4, 1.2, 2.7, 3.13};
    for (const auto delay : delays) {
        const auto design = pulp::signal::design_thiran1(delay);
        REQUIRE(design.valid);
        CHECK(std::abs(design.feedback) <= 1.0 / 3.0);
        for (const auto omega : omegas) {
            const auto oracle = independent_thiran_response(delay, omega);
            CHECK_THAT(std::abs(oracle), WithinAbs(1.0, 2.0e-15));
            CHECK_THAT(pulp::signal::thiran1_magnitude(design, omega), WithinAbs(1.0, 0.0));
            CHECK_THAT(pulp::signal::thiran1_group_delay_samples(design, omega),
                       WithinAbs(independent_group_delay(delay, omega), 2.0e-8));
        }
        CHECK_THAT(pulp::signal::thiran1_group_delay_samples(design, 0.0),
                   WithinAbs(delay, 2.0e-15));
    }
    CHECK_FALSE(pulp::signal::design_thiran1(0.999).valid);
    CHECK_FALSE(pulp::signal::design_thiran1(2.001).valid);
}

TEST_CASE("Lagrange-5 reproduces every polynomial through degree five",
          "[signal][fractional-delay]") {
    constexpr std::array nodes{-2.0, -1.0, 0.0, 1.0, 2.0, 3.0};
    for (const auto fraction : {0.0, 0.001, 0.25, 0.5, 0.875, 1.0}) {
        const auto weights = pulp::signal::lagrange5_weights(fraction);
        for (int degree = 0; degree <= 5; ++degree) {
            double interpolated = 0.0;
            for (std::size_t i = 0; i < nodes.size(); ++i)
                interpolated += weights[i] * std::pow(nodes[i], degree);
            CHECK_THAT(interpolated, WithinAbs(std::pow(fraction, degree), 3.0e-14));
            CHECK_THAT(pulp::signal::lagrange5(
                           fraction, std::pow(nodes[0], degree), std::pow(nodes[1], degree),
                           std::pow(nodes[2], degree), std::pow(nodes[3], degree),
                           std::pow(nodes[4], degree), std::pow(nodes[5], degree)),
                       WithinAbs(std::pow(fraction, degree), 3.0e-14));
        }
    }

    constexpr std::array order3_nodes{-1.0, 0.0, 1.0, 2.0};
    for (const auto fraction : {0.0, 0.125, 0.5, 0.999}) {
        for (int degree = 0; degree <= 3; ++degree) {
            const auto value = pulp::signal::Interpolator::lagrange(
                fraction, std::pow(order3_nodes[0], degree), std::pow(order3_nodes[1], degree),
                std::pow(order3_nodes[2], degree), std::pow(order3_nodes[3], degree));
            CHECK_THAT(value, WithinAbs(std::pow(fraction, degree), 2.0e-14));
        }
    }
}

TEST_CASE("maximum delay includes the full cold-history stencil without wrapping",
          "[signal][fractional-delay]") {
    FractionalDelayLineT<double> line;
    REQUIRE(line.prepare(5, FractionalDelayMethod::lagrange5));
    CHECK(line.retained_samples() == 9);
    for (std::size_t n = 0; n < 12; ++n) {
        const auto result = line.process(n == 0 ? 1.0 : 0.0, 5.0);
        REQUIRE(result);
        CHECK_THAT(result.sample, WithinAbs(n == 5 ? 1.0 : 0.0, 1.0e-15));
    }
}

TEST_CASE("all methods use push-then-read timing and canonical integer taps",
          "[signal][fractional-delay]") {
    RoundingModeGuard rounding_mode;
    REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
    for (const auto method : {FractionalDelayMethod::thiran1, FractionalDelayMethod::lagrange3,
                              FractionalDelayMethod::lagrange5}) {
        FractionalDelayLineT<double> line;
        if (method == FractionalDelayMethod::thiran1)
            REQUIRE(line.prepare_thiran1(16, 4));
        else
            REQUIRE(line.prepare(16, method));
        const auto delay = std::nextafter(4.0, 3.0);
        for (std::size_t n = 0; n < 12; ++n) {
            const auto result = line.process(n == 0 ? 1.0 : 0.0, delay);
            REQUIRE(result);
            CHECK_THAT(result.sample, WithinAbs(n == 4 ? 1.0 : 0.0, 1.0e-14));
        }
        CHECK(line.processing_latency_samples() == 0);
        CHECK(line.required_older_lookback() ==
              16 + FractionalDelayLineT<double>::older_stencil_lookback(method));
    }
}

TEST_CASE("Lagrange-5 improves high-frequency error over Lagrange-3",
          "[signal][fractional-delay]") {
    constexpr std::size_t count = 8192;
    constexpr double omega = 0.72 * std::numbers::pi;
    constexpr double delay = 7.37;
    std::vector<double> input(count);
    std::vector<double> delays(count, delay);
    for (std::size_t n = 0; n < count; ++n)
        input[n] = std::cos(omega * static_cast<double>(n));
    const auto order3 = render(FractionalDelayMethod::lagrange3, input, delays);
    const auto order5 = render(FractionalDelayMethod::lagrange5, input, delays);
    const auto error3 = fitted_complex_error(order3, omega, delay, 128);
    const auto error5 = fitted_complex_error(order5, omega, delay, 128);
    CHECK(error5 < error3);
    CHECK(error5 < 0.3);
}

TEST_CASE("Thiran endpoint retuning stays bounded without hidden smoothing",
          "[signal][fractional-delay]") {
    FractionalDelayLineT<double> line;
    REQUIRE(line.prepare(8, FractionalDelayMethod::thiran1));
    const auto level = std::numeric_limits<double>::max() * 0.125;
    bool all_ok = true;
    double peak = 0.0;
    for (std::size_t n = 0; n < 20000; ++n) {
        const auto delay =
            (n & 1u) == 0 ? 1.0 : 2.0 - 64.0 * std::numeric_limits<double>::epsilon();
        const auto input = (n & 2u) == 0 ? level : -level;
        const auto result = line.process(input, delay);
        all_ok = all_ok && static_cast<bool>(result) && std::isfinite(result.sample);
        peak = std::max(peak, std::abs(result.sample));
    }
    CHECK(all_ok);
    CHECK(peak <= level * 2.0);
}

TEST_CASE("Thiran modulation is confined to its prepared integer interval",
          "[signal][fractional-delay]") {
    FractionalDelayLineT<double> line;
    REQUIRE(line.prepare_thiran1(12, 4));
    CHECK(line.thiran_integer_interval_start() == 4);

    constexpr auto epsilon = std::numeric_limits<double>::epsilon();
    const auto below_lower = 4.0 - 64.0 * epsilon;
    const auto above_lower = 4.0 + 64.0 * epsilon;
    const auto below_upper = 5.0 - 64.0 * epsilon;
    const auto above_upper = 5.0 + 64.0 * epsilon;
    CHECK(line.process(1.0, below_lower).status == FractionalDelayStatus::invalid_delay);
    REQUIRE(line.process(1.0, 4.0));
    REQUIRE(line.process(1.0, above_lower));
    REQUIRE(line.process(1.0, below_upper));
    CHECK(line.process(1.0, std::nextafter(5.0, 4.0)).status ==
          FractionalDelayStatus::invalid_delay);
    CHECK(line.process(1.0, 5.0).status == FractionalDelayStatus::invalid_delay);
    CHECK(line.process(1.0, above_upper).status == FractionalDelayStatus::invalid_delay);

    std::array<double, 4> input{0.25, 0.5, 0.75, 1.0};
    std::array<double, 4> delays{4.25, 5.0, 4.5, 3.75};
    std::array<double, 4> output{};
    const auto block = line.process(input.data(), output.data(), delays.data(), input.size());
    CHECK(block.status == FractionalDelayStatus::invalid_delay);
    CHECK(block.processed_frames == input.size());
    CHECK(block.fault_count == 2);
    CHECK(output[1] == 0.0);
    CHECK(output[3] == 0.0);
    const auto recovered = line.process(0.125, 4.5);
    REQUIRE(recovered);
    CHECK(std::isfinite(recovered.sample));
}

TEST_CASE("reset, partitioning, and exact in-place processing are deterministic",
          "[signal][fractional-delay]") {
    std::vector<float> input(257);
    std::vector<double> delays(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<float>(std::sin(0.071 * static_cast<double>(i)));
        delays[i] = 5.0 + 0.9 * std::sin(0.013 * static_cast<double>(i));
    }

    FractionalDelayLineT<float> whole;
    REQUIRE(whole.prepare(16, FractionalDelayMethod::lagrange5));
    std::vector<float> expected(input.size());
    REQUIRE(whole.process(input.data(), expected.data(), delays.data(), input.size()));

    FractionalDelayLineT<float> partitioned;
    REQUIRE(partitioned.prepare(16, FractionalDelayMethod::lagrange5));
    std::vector<float> actual = input;
    REQUIRE(partitioned.process(actual.data(), actual.data(), delays.data(), 73));
    REQUIRE(partitioned.process(actual.data() + 73, actual.data() + 73, delays.data() + 73,
                                actual.size() - 73));
    CHECK(actual == expected);

    partitioned.reset();
    actual = input;
    REQUIRE(partitioned.process(actual.data(), actual.data(), delays.data(), actual.size()));
    CHECK(actual == expected);
}

TEST_CASE("faults inject zero, advance time, and recover with typed status",
          "[signal][fractional-delay]") {
    FractionalDelayLineT<float> line;
    REQUIRE(line.prepare(8, FractionalDelayMethod::lagrange3));
    REQUIRE(line.process(1.0f, 1.0));
    const auto bad_input = line.process(std::numeric_limits<float>::quiet_NaN(), 1.0);
    CHECK(bad_input.status == FractionalDelayStatus::non_finite_input);
    CHECK(bad_input.sample == 0.0f);
    const auto bad_delay = line.process(1.0f, std::numeric_limits<double>::infinity());
    CHECK(bad_delay.status == FractionalDelayStatus::invalid_delay);
    CHECK(bad_delay.sample == 0.0f);
    const auto beyond_size_t =
        line.process(1.0f, static_cast<double>(std::numeric_limits<std::size_t>::max()));
    CHECK(beyond_size_t.status == FractionalDelayStatus::invalid_delay);
    CHECK(beyond_size_t.sample == 0.0f);
    const auto recovered = line.process(0.25f, 1.0);
    REQUIRE(recovered);
    CHECK(std::isfinite(recovered.sample));
    CHECK(recovered.sample == 0.0f);

    std::array<float, 2> input{0.5f, 0.25f};
    std::array<float, 2> output{9.0f, 9.0f};
    const auto null_result = line.process(nullptr, output.data(), input.size(), 2.0);
    CHECK(null_result.status == FractionalDelayStatus::invalid_argument);
    CHECK(null_result.processed_frames == 0);
    CHECK(output == std::array<float, 2>{9.0f, 9.0f});
}

TEST_CASE("derived overflow injects zero and recovers without poisoning history",
          "[signal][fractional-delay]") {
    FractionalDelayLineT<float> line;
    REQUIRE(line.prepare(8, FractionalDelayMethod::lagrange5));
    const auto maximum = std::numeric_limits<float>::max();
    for (const auto sample : {maximum, -maximum, maximum, maximum, -maximum})
        REQUIRE(line.process(sample, 2.0));
    const auto overflow = line.process(maximum, 2.5);
    CHECK(overflow.status == FractionalDelayStatus::output_out_of_range);
    CHECK(overflow.sample == 0.0f);
    for (int i = 0; i < 8; ++i) {
        const auto recovered = line.process(0.0f, 2.0);
        REQUIRE(recovered);
        CHECK(std::isfinite(recovered.sample));
    }
}

TEST_CASE("prepare is transactional and processing allocates nothing",
          "[signal][fractional-delay]") {
    FractionalDelayLineT<double> line;
    CHECK_FALSE(line.prepared());
    CHECK_FALSE(line.method().has_value());
    CHECK_FALSE(line.thiran_integer_interval_start().has_value());
    CHECK(line.maximum_delay_samples() == 0);
    CHECK(line.minimum_delay_samples() == 0);
    CHECK(line.required_older_lookback() == 0);
    REQUIRE(line.prepare_thiran1(32, 3));
    CHECK(line.thiran_integer_interval_start() == 3);
    const auto retained = line.retained_samples();
    CHECK_FALSE(line.prepare(0, FractionalDelayMethod::lagrange5));
    CHECK(line.prepared());
    CHECK(line.method() == FractionalDelayMethod::thiran1);
    CHECK(line.retained_samples() == retained);
    const auto unknown = static_cast<FractionalDelayMethod>(999);
    CHECK_FALSE(line.prepare(32, unknown));
    CHECK_FALSE(line.prepare_thiran1(3, 3));
    CHECK(FractionalDelayLineT<double>::minimum_delay_samples(unknown) == 0);
    CHECK(FractionalDelayLineT<double>::older_stencil_lookback(unknown) == 0);
    CHECK(line.method() == FractionalDelayMethod::thiran1);

    std::array<double, 128> input{};
    std::array<double, 128> output{};
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(line.process(input.data(), output.data(), input.size(), 3.5));
        CHECK(probe.allocation_count() == 0);
    }
}
