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
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::FractionalDelayHistoryT;
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

std::complex<long double> independent_lagrange_response(FractionalDelayMethod method, double delay,
                                                        double omega) {
    const auto integer = static_cast<long long>(std::floor(delay));
    const auto fraction = static_cast<long double>(delay - static_cast<double>(integer));
    constexpr std::array<long long, 4> order3_nodes{-1, 0, 1, 2};
    constexpr std::array<long long, 6> order5_nodes{-2, -1, 0, 1, 2, 3};
    std::complex<long double> response{};
    const auto accumulate = [&](const auto& nodes) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            long double weight = 1.0L;
            for (std::size_t j = 0; j < nodes.size(); ++j) {
                if (i != j)
                    weight *= (fraction - static_cast<long double>(nodes[j])) /
                              static_cast<long double>(nodes[i] - nodes[j]);
            }
            const auto tap_delay = static_cast<long double>(integer + nodes[i]);
            response += weight * std::exp(std::complex<long double>{
                                     0.0L, -static_cast<long double>(omega) * tap_delay});
        }
    };
    if (method == FractionalDelayMethod::lagrange3)
        accumulate(order3_nodes);
    else
        accumulate(order5_nodes);
    return response;
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

TEST_CASE("shared history has transactional bounded preparation and typed faults",
          "[signal][fractional-delay][shared-history]") {
    FractionalDelayHistoryT<double> history;
    CHECK_FALSE(history.prepared());
    CHECK(history.maximum_delay_samples() == 0);
    CHECK(history.retained_samples() == 0);
    CHECK(history.retained_bytes() == 0);
    CHECK(history.required_older_lookback() == 0);
    CHECK(history.push(1.0) == FractionalDelayStatus::not_prepared);
    CHECK(history.read_lagrange3_at(2.0).status == FractionalDelayStatus::not_prepared);

    REQUIRE(history.prepare(16));
    CHECK(history.maximum_delay_samples() == 16);
    CHECK(history.retained_samples() == 19);
    CHECK(history.retained_bytes() == 19 * sizeof(double));
    CHECK(history.required_older_lookback() == 19);
    CHECK(FractionalDelayHistoryT<double>::minimum_delay_samples(
              FractionalDelayMethod::lagrange3) == 2);
    CHECK(FractionalDelayHistoryT<double>::minimum_delay_samples(
              FractionalDelayMethod::lagrange5) == 3);
    CHECK(FractionalDelayHistoryT<double>::minimum_delay_samples(FractionalDelayMethod::thiran1) ==
          0);

    REQUIRE(history.push(0.5) == FractionalDelayStatus::ok);
    const auto retained = history.retained_samples();
    CHECK_FALSE(history.prepare(2));
    CHECK_FALSE(history.prepare(std::numeric_limits<std::size_t>::max()));
    CHECK(history.prepared());
    CHECK(history.maximum_delay_samples() == 16);
    CHECK(history.retained_samples() == retained);
    CHECK_THAT(history.read_lagrange3_at(2.0).sample, WithinAbs(0.0, 0.0));

    CHECK(history.read_at(4.0, FractionalDelayMethod::thiran1).status ==
          FractionalDelayStatus::invalid_argument);
    CHECK(history.read_lagrange3_at(1.999).status == FractionalDelayStatus::invalid_delay);
    CHECK(history.read_lagrange5_at(2.999).status == FractionalDelayStatus::invalid_delay);
    CHECK(history.read_lagrange3_at(16.001).status == FractionalDelayStatus::invalid_delay);
    CHECK(history.read_lagrange5_at(std::numeric_limits<double>::quiet_NaN()).status ==
          FractionalDelayStatus::invalid_delay);
    CHECK(history.read_lagrange5_at(std::numeric_limits<double>::infinity()).status ==
          FractionalDelayStatus::invalid_delay);
    CHECK(history.read_lagrange5_at(static_cast<double>(std::numeric_limits<std::size_t>::max()))
              .status == FractionalDelayStatus::invalid_delay);

    CHECK(history.push(std::numeric_limits<double>::quiet_NaN()) ==
          FractionalDelayStatus::non_finite_input);
    REQUIRE(history.push(0.25) == FractionalDelayStatus::ok);
    CHECK(std::isfinite(history.read_lagrange3_at(2.0).sample));
    history.reset();
    CHECK(history.read_lagrange3_at(2.0).sample == 0.0);

    std::array<double, 256> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(0.03 * static_cast<double>(i));
    {
        pulp::test::RtAllocationProbe probe;
        for (const auto sample : input) {
            REQUIRE(history.push(sample) == FractionalDelayStatus::ok);
            REQUIRE(history.read_lagrange3_at(4.25));
            REQUIRE(history.read_lagrange5_at(11.75));
        }
        CHECK(probe.allocation_count() == 0);
    }
}

TEST_CASE("shared history read heads observe one exact immutable snapshot",
          "[signal][fractional-delay][shared-history]") {
    FractionalDelayHistoryT<double> history;
    REQUIRE(history.prepare(32));
    for (int sample = 1; sample <= 24; ++sample)
        REQUIRE(history.push(static_cast<double>(sample)) == FractionalDelayStatus::ok);

    const auto old_l3 = history.read_lagrange3_at(4.25);
    const auto old_l5 = history.read_lagrange5_at(9.75);
    const auto integer_head = history.read_lagrange5_at(3.0);
    REQUIRE(old_l3);
    REQUIRE(old_l5);
    REQUIRE(integer_head);
    CHECK_THAT(old_l3.sample, WithinAbs(20.75, 2.0e-14));
    CHECK_THAT(old_l5.sample, WithinAbs(15.25, 2.0e-14));
    CHECK(integer_head.sample == 22.0);

    for (int repeat = 0; repeat < 8; ++repeat) {
        CHECK(history.read_lagrange3_at(4.25).sample == old_l3.sample);
        CHECK(history.read_lagrange5_at(9.75).sample == old_l5.sample);
        CHECK(history.read_lagrange5_at(3.0).sample == integer_head.sample);
    }

    REQUIRE(history.push(25.0) == FractionalDelayStatus::ok);
    CHECK_THAT(history.read_lagrange3_at(4.25).sample, WithinAbs(21.75, 2.0e-14));
    CHECK_THAT(history.read_lagrange5_at(9.75).sample, WithinAbs(16.25, 2.0e-14));
}

TEST_CASE("shared history supports identical float and double cursor contracts",
          "[signal][fractional-delay][shared-history]") {
    const auto exercise = []<typename SampleType>() {
        FractionalDelayHistoryT<SampleType> history;
        REQUIRE(history.prepare(12));
        for (int sample = 1; sample <= 10; ++sample)
            REQUIRE(history.push(static_cast<SampleType>(sample)) == FractionalDelayStatus::ok);
        const auto order3 = history.read_lagrange3_at(4.0);
        const auto order5 = history.read_lagrange5_at(7.0);
        REQUIRE(order3);
        REQUIRE(order5);
        CHECK(order3.sample == static_cast<SampleType>(7));
        CHECK(order5.sample == static_cast<SampleType>(4));
        history.reset();
        CHECK(history.read_lagrange3_at(4.0).sample == SampleType{});
        CHECK(history.read_lagrange5_at(7.0).sample == SampleType{});
    };
    exercise.template operator()<float>();
    exercise.template operator()<double>();
}

TEST_CASE("shared history moves preserve destination state and empty the source",
          "[signal][fractional-delay][shared-history]") {
    FractionalDelayHistoryT<double> source;
    REQUIRE(source.prepare(16));
    for (int sample = 1; sample <= 12; ++sample)
        REQUIRE(source.push(static_cast<double>(sample)) == FractionalDelayStatus::ok);

    FractionalDelayHistoryT<double> moved{std::move(source)};
    CHECK_FALSE(source.prepared());
    CHECK(source.maximum_delay_samples() == 0);
    CHECK(source.retained_samples() == 0);
    CHECK(source.retained_bytes() == 0);
    CHECK(source.required_older_lookback() == 0);
    CHECK(source.push(1.0) == FractionalDelayStatus::not_prepared);
    CHECK(source.read_lagrange3_at(4.0).status == FractionalDelayStatus::not_prepared);
    REQUIRE(moved.prepared());
    CHECK(moved.read_lagrange3_at(4.0).sample == 9.0);

    FractionalDelayHistoryT<double> assigned;
    REQUIRE(assigned.prepare(4));
    REQUIRE(assigned.push(-1.0) == FractionalDelayStatus::ok);
    assigned = std::move(moved);
    CHECK_FALSE(moved.prepared());
    CHECK(moved.retained_samples() == 0);
    CHECK(moved.push(1.0) == FractionalDelayStatus::not_prepared);
    REQUIRE(assigned.prepared());
    CHECK(assigned.maximum_delay_samples() == 16);
    CHECK(assigned.read_lagrange5_at(7.0).sample == 6.0);
}

TEST_CASE("shared Lagrange heads are continuous across integer boundaries and partition exact",
          "[signal][fractional-delay][shared-history]") {
    constexpr std::size_t count = 513;
    std::array<double, count> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(0.071 * static_cast<double>(i)) +
                   0.25 * std::cos(0.019 * static_cast<double>(i));

    const auto render_shared = [&](const std::array<std::size_t, 3>& partitions) {
        FractionalDelayHistoryT<double> history;
        REQUIRE(history.prepare(32));
        std::array<double, count> output{};
        std::size_t begin = 0;
        for (const auto end : partitions) {
            for (auto i = begin; i < end; ++i) {
                REQUIRE(history.push(input[i]) == FractionalDelayStatus::ok);
                const auto result =
                    history.read_lagrange5_at(8.0 + 0.9 * std::sin(0.017 * static_cast<double>(i)));
                REQUIRE(result);
                output[i] = result.sample;
            }
            begin = end;
        }
        return output;
    };

    const auto whole = render_shared({count, count, count});
    const auto partitioned = render_shared({73, 251, count});
    CHECK(whole == partitioned);

    FractionalDelayHistoryT<double> history;
    REQUIRE(history.prepare(32));
    for (const auto sample : input)
        REQUIRE(history.push(sample) == FractionalDelayStatus::ok);
    constexpr double epsilon = 1.0e-7;
    for (const auto method : {FractionalDelayMethod::lagrange3, FractionalDelayMethod::lagrange5}) {
        const auto below = history.read_at(9.0 - epsilon, method);
        const auto exact = history.read_at(9.0, method);
        const auto above = history.read_at(9.0 + epsilon, method);
        REQUIRE(below);
        REQUIRE(exact);
        REQUIRE(above);
        CHECK(std::abs(below.sample - exact.sample) < 1.0e-7);
        CHECK(std::abs(above.sample - exact.sample) < 1.0e-7);
        CHECK(std::abs((below.sample + above.sample) * 0.5 - exact.sample) < 1.0e-12);
    }
}

TEST_CASE("read-before-write feedback uses the requested impulse loop delay",
          "[signal][fractional-delay][shared-history]") {
    FractionalDelayHistoryT<double> history;
    REQUIRE(history.prepare(16));
    constexpr std::size_t loop_delay = 4;
    constexpr double feedback = 0.5;
    std::array<double, 21> output{};
    for (std::size_t n = 0; n < output.size(); ++n) {
        const auto loop = history.read_lagrange3_at(static_cast<double>(loop_delay));
        REQUIRE(loop);
        output[n] = (n == 0 ? 1.0 : 0.0) + feedback * loop.sample;
        REQUIRE(history.push(output[n]) == FractionalDelayStatus::ok);
    }
    for (std::size_t n = 0; n < output.size(); ++n) {
        const auto expected = n % loop_delay == 0 ? std::pow(feedback, n / loop_delay) : 0.0;
        CHECK(output[n] == expected);
    }
}

TEST_CASE("shared Lagrange magnitude and phase match an independent fractional oracle",
          "[signal][fractional-delay][shared-history]") {
    constexpr std::array fractions{0.1, 0.25, 0.5, 0.9};
    constexpr std::array omegas{0.1 * std::numbers::pi, 0.45 * std::numbers::pi,
                                0.8 * std::numbers::pi};
    for (const auto method : {FractionalDelayMethod::lagrange3, FractionalDelayMethod::lagrange5}) {
        for (const auto fraction : fractions) {
            const auto physical_delay = 7.0 + fraction;
            // The shared-history cursor is the next write, so a read performed
            // after pushing x[n] uses one additional sample of cursor distance.
            const auto requested_delay = physical_delay + 1.0;
            for (const auto omega : omegas) {
                FractionalDelayHistoryT<double> cosine_history;
                FractionalDelayHistoryT<double> sine_history;
                REQUIRE(cosine_history.prepare(32));
                REQUIRE(sine_history.prepare(32));
                std::complex<long double> measured{};
                constexpr std::size_t measure_at = 160;
                for (std::size_t n = 0; n <= measure_at; ++n) {
                    const auto phase = omega * static_cast<double>(n);
                    REQUIRE(cosine_history.push(std::cos(phase)) == FractionalDelayStatus::ok);
                    REQUIRE(sine_history.push(std::sin(phase)) == FractionalDelayStatus::ok);
                    if (n == measure_at) {
                        const auto real = cosine_history.read_at(requested_delay, method);
                        const auto imaginary = sine_history.read_at(requested_delay, method);
                        REQUIRE(real);
                        REQUIRE(imaginary);
                        measured = {real.sample, imaginary.sample};
                    }
                }
                const auto carrier = std::exp(std::complex<long double>{
                    0.0L, static_cast<long double>(omega * static_cast<double>(measure_at))});
                const auto measured_response = measured / carrier;
                const auto oracle = independent_lagrange_response(method, physical_delay, omega);
                CHECK_THAT(static_cast<double>(std::abs(measured_response)),
                           WithinAbs(static_cast<double>(std::abs(oracle)), 2.0e-13));
                CHECK_THAT(static_cast<double>(std::arg(measured_response / oracle)),
                           WithinAbs(0.0, 2.0e-13));
            }
        }
    }
}
