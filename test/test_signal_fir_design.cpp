#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/fir_design.hpp>
#include <pulp/signal/fir_filter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

std::complex<double> direct_dft_response_at(std::span<const double> impulse, double omega) {
    std::complex<double> response{};
    for (std::size_t sample = 0u; sample < impulse.size(); ++sample)
        response += impulse[sample] * std::polar(1.0, -omega * static_cast<double>(sample));
    return response;
}

double direct_signed_zero_phase_amplitude(std::span<const double> coefficients,
                                          LinearPhaseFirType type, double omega) {
    const double delay = 0.5 * static_cast<double>(coefficients.size() - 1u);
    const auto zero_phase =
        direct_dft_response_at(coefficients, omega) * std::polar(1.0, delay * omega);
    const bool antisymmetric = type == LinearPhaseFirType::type_iii_antisymmetric_odd ||
                               type == LinearPhaseFirType::type_iv_antisymmetric_even;
    return antisymmetric ? zero_phase.imag() : zero_phase.real();
}

std::vector<FirDesignPoint> sampled_target(std::span<const double> coefficients,
                                           LinearPhaseFirType type, std::size_t count = 97u) {
    std::vector<FirDesignPoint> points;
    points.reserve(count);
    for (std::size_t index = 0u; index < count; ++index) {
        const double omega = kPi * static_cast<double>(index) / static_cast<double>(count - 1u);
        points.push_back({omega, direct_signed_zero_phase_amplitude(coefficients, type, omega),
                          0.25 + static_cast<double>((index % 7u) + 1u)});
    }
    // Ordering is deliberately irrelevant to the fit.
    std::rotate(points.begin(), points.begin() + 31, points.end());
    return points;
}

void require_recovery(std::initializer_list<double> expected_values, LinearPhaseFirType type) {
    const std::vector<double> expected(expected_values);
    auto points = sampled_target(expected, type);
    const auto result =
        design_fir_least_squares(points, {.tap_count = expected.size(), .type = type});
    REQUIRE(result.status == FirDesignStatus::success);
    REQUIRE(result.numerical_rank > 0u);
    REQUIRE(result.qr_diagonal_condition_estimate >= 1.0);
    REQUIRE(result.weighted_rms_error < 1.0e-12);
    REQUIRE(result.maximum_absolute_error < 1.0e-11);
    REQUIRE(result.coefficients.size() == expected.size());
    for (std::size_t index = 0u; index < expected.size(); ++index)
        REQUIRE_THAT(result.coefficients[index], WithinAbs(expected[index], 1.0e-12));
    constexpr double non_endpoint = 0.73;
    REQUIRE_THAT(
        linear_phase_fir_amplitude(expected, type, non_endpoint),
        WithinAbs(direct_signed_zero_phase_amplitude(expected, type, non_endpoint), 1.0e-14));
}

double weighted_squared_residual(std::span<const FirDesignPoint> points,
                                 std::span<const double> coefficients, LinearPhaseFirType type) {
    double residual = 0.0;
    for (const auto& point : points) {
        const double error =
            direct_signed_zero_phase_amplitude(coefficients, type, point.omega) - point.amplitude;
        residual += point.weight * error * error;
    }
    return residual;
}

} // namespace

TEST_CASE("least-squares FIR exactly recovers all four linear-phase types",
          "[signal][fir-design][least-squares]") {
    SECTION("Type I: odd symmetric") {
        require_recovery({0.06, -0.12, 0.28, 0.56, 0.28, -0.12, 0.06},
                         LinearPhaseFirType::type_i_symmetric_odd);
    }
    SECTION("Type II: even symmetric") {
        require_recovery({-0.04, 0.18, 0.36, 0.36, 0.18, -0.04},
                         LinearPhaseFirType::type_ii_symmetric_even);
    }
    SECTION("Type III: odd antisymmetric") {
        require_recovery({0.08, -0.22, 0.35, 0.0, -0.35, 0.22, -0.08},
                         LinearPhaseFirType::type_iii_antisymmetric_odd);
    }
    SECTION("Type IV: even antisymmetric") {
        require_recovery({0.05, -0.16, 0.41, -0.41, 0.16, -0.05},
                         LinearPhaseFirType::type_iv_antisymmetric_even);
    }
}

TEST_CASE("linear-phase FIR types enforce symmetry and analytic endpoint invariants",
          "[signal][fir-design][least-squares]") {
    const std::vector<double> type_i{0.1, 0.2, 0.4, 0.2, 0.1};
    const std::vector<double> type_ii{0.1, 0.4, 0.4, 0.1};
    const std::vector<double> type_iii{0.2, 0.4, 0.0, -0.4, -0.2};
    const std::vector<double> type_iv{0.2, 0.4, -0.4, -0.2};

    REQUIRE_THAT(linear_phase_fir_amplitude(type_i, LinearPhaseFirType::type_i_symmetric_odd, 0.0),
                 WithinAbs(1.0, 1.0e-14));
    REQUIRE(linear_phase_fir_amplitude(
                type_ii, LinearPhaseFirType::type_ii_symmetric_even, kPi) == 0.0);
    REQUIRE(linear_phase_fir_amplitude(
                type_iii, LinearPhaseFirType::type_iii_antisymmetric_odd, 0.0) == 0.0);
    REQUIRE(linear_phase_fir_amplitude(
                type_iii, LinearPhaseFirType::type_iii_antisymmetric_odd, kPi) == 0.0);
    REQUIRE(linear_phase_fir_amplitude(
                type_iv, LinearPhaseFirType::type_iv_antisymmetric_even, 0.0) == 0.0);
    REQUIRE(std::abs(linear_phase_fir_amplitude(
                type_iv, LinearPhaseFirType::type_iv_antisymmetric_even, kPi)) > 0.1);
}

TEST_CASE("least-squares weights select the intended compromise and report its error",
          "[signal][fir-design][least-squares]") {
    const std::vector<FirDesignPoint> points{{0.0, 0.0, 1.0}, {kPi, 10.0, 9.0}};
    const auto result = design_fir_least_squares(
        points, {.tap_count = 1u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(result);
    REQUIRE_THAT(result.coefficients[0], WithinAbs(9.0, 1.0e-13));
    REQUIRE_THAT(result.measured_amplitudes[0], WithinAbs(9.0, 1.0e-13));
    REQUIRE_THAT(result.errors[1], WithinAbs(-1.0, 1.0e-13));
    REQUIRE_THAT(result.weighted_rms_error, WithinAbs(3.0, 1.0e-13));
    REQUIRE_THAT(result.maximum_absolute_error, WithinAbs(9.0, 1.0e-13));
}

TEST_CASE("least-squares preserves representable weight ratios across the finite range",
          "[signal][fir-design][least-squares][regression]") {
    const std::vector<FirDesignPoint> points{
        {0.0, 1.0e300, 1.0e-300}, {kPi, 0.0, 1.0e100}};
    const auto result = design_fir_least_squares(
        points, {.tap_count = 1u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(result);
    REQUIRE(result.coefficients[0] > 0.9e-100);
    REQUIRE(result.coefficients[0] < 1.1e-100);
    REQUIRE(result.weighted_rms_error > 0.9e100);
    REQUIRE(result.weighted_rms_error < 1.1e100);
}

TEST_CASE("Type I hand solution and dense direct response agree",
          "[signal][fir-design][least-squares][oracle]") {
    // For three symmetric taps [s, c, s], H(0)=c+2s and H(pi)=c-2s.
    // The exact two-equation solution H(0)=1, H(pi)=0 is [0.25, 0.5, 0.25].
    const std::vector<FirDesignPoint> points{{0.0, 1.0, 1.0}, {kPi, 0.0, 1.0}};
    const auto result = design_fir_least_squares(
        points, {.tap_count = 3u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(result);
    REQUIRE_THAT(result.coefficients[0], WithinAbs(0.25, 1.0e-14));
    REQUIRE_THAT(result.coefficients[1], WithinAbs(0.5, 1.0e-14));
    REQUIRE_THAT(result.coefficients[2], WithinAbs(0.25, 1.0e-14));

    for (std::size_t bin = 0u; bin < 2049u; ++bin) {
        const double omega = kPi * static_cast<double>(bin) / 2048.0;
        const double hand_target = 0.5 + 0.5 * std::cos(omega);
        REQUIRE_THAT(std::abs(direct_dft_response_at(result.coefficients, omega)),
                     WithinAbs(hand_target, 2.0e-14));
    }
}

TEST_CASE("weight omission and hidden normalization mutations fail the independent objective",
          "[signal][fir-design][least-squares][negative-control]") {
    const std::vector<FirDesignPoint> points{{0.0, 0.0, 1.0}, {kPi, 10.0, 9.0}};
    const auto result = design_fir_least_squares(
        points, {.tap_count = 1u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(result);

    const std::array<double, 1> weight_omitted{5.0};
    const std::array<double, 1> normalized_to_unit_dc{1.0};
    REQUIRE(weighted_squared_residual(points, result.coefficients,
                                      LinearPhaseFirType::type_i_symmetric_odd) < 91.0);
    REQUIRE(weighted_squared_residual(points, weight_omitted,
                                      LinearPhaseFirType::type_i_symmetric_odd) > 249.0);
    REQUIRE(weighted_squared_residual(points, normalized_to_unit_dc,
                                      LinearPhaseFirType::type_i_symmetric_odd) > 700.0);
}

TEST_CASE("least-squares FIR rejects invalid parity, rank loss, conditioning, and budgets",
          "[signal][fir-design][least-squares][fault]") {
    const std::vector<FirDesignPoint> healthy{
        {0.0, 1.0, 1.0}, {0.4, 0.8, 1.0}, {1.2, 0.2, 1.0}, {kPi, 0.0, 1.0}};
    auto bad_parity = design_fir_least_squares(
        healthy, {.tap_count = 4u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(bad_parity.status == FirDesignStatus::invalid_argument);

    auto too_many_taps =
        design_fir_least_squares(healthy, {.tap_count = kMaximumFirDesignTapCount + 2u,
                                           .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(too_many_taps.status == FirDesignStatus::unsupported_size);
    const std::vector<FirDesignPoint> too_many_points(kMaximumFirDesignPointCount + 1u,
                                                      FirDesignPoint{0.3, 1.0, 1.0});
    REQUIRE(
        design_fir_least_squares(
            too_many_points, {.tap_count = 1u, .type = LinearPhaseFirType::type_i_symmetric_odd})
            .status == FirDesignStatus::unsupported_size);

    const std::vector<FirDesignPoint> duplicate(4u, FirDesignPoint{0.3, 1.0, 1.0});
    auto rank_lost = design_fir_least_squares(
        duplicate, {.tap_count = 5u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(rank_lost.status == FirDesignStatus::rank_deficient);
    REQUIRE(rank_lost.numerical_rank < 3u);

    const std::vector<FirDesignPoint> incompatible_type_ii_endpoint{{kPi, 1.0, 1.0}};
    REQUIRE(design_fir_least_squares(
                incompatible_type_ii_endpoint,
                {.tap_count = 2u, .type = LinearPhaseFirType::type_ii_symmetric_even})
                .status == FirDesignStatus::rank_deficient);
    const std::vector<FirDesignPoint> incompatible_type_iii_endpoint{{0.0, 1.0, 1.0}};
    REQUIRE(design_fir_least_squares(
                incompatible_type_iii_endpoint,
                {.tap_count = 3u, .type = LinearPhaseFirType::type_iii_antisymmetric_odd})
                .status == FirDesignStatus::rank_deficient);

    auto ill_conditioned =
        design_fir_least_squares(healthy, {.tap_count = 5u,
                                           .type = LinearPhaseFirType::type_i_symmetric_odd,
                                           .maximum_diagonal_condition_estimate = 1.0});
    REQUIRE(ill_conditioned.status == FirDesignStatus::ill_conditioned);
    REQUIRE(ill_conditioned.qr_diagonal_condition_estimate > 1.0);

    auto over_budget =
        design_fir_least_squares(healthy, {.tap_count = 5u,
                                           .type = LinearPhaseFirType::type_i_symmetric_odd,
                                           .maximum_workspace_bytes = 8u});
    REQUIRE(over_budget.status == FirDesignStatus::workspace_limit_exceeded);

    auto invalid_point = healthy;
    invalid_point[0].weight = 0.0;
    REQUIRE(design_fir_least_squares(
                invalid_point, {.tap_count = 5u, .type = LinearPhaseFirType::type_i_symmetric_odd})
                .status == FirDesignStatus::invalid_argument);
    invalid_point = healthy;
    invalid_point[0].omega = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(design_fir_least_squares(
                invalid_point, {.tap_count = 5u, .type = LinearPhaseFirType::type_i_symmetric_odd})
                .status == FirDesignStatus::invalid_argument);

    const std::vector<FirDesignPoint> extreme{
        {0.0, std::numeric_limits<double>::max(), std::numeric_limits<double>::max()}};
    const auto overflow = design_fir_least_squares(
        extreme, {.tap_count = 1u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(overflow.status == FirDesignStatus::numerical_failure);
    REQUIRE(overflow.coefficients.empty());
    REQUIRE(overflow.measured_amplitudes.empty());
    REQUIRE(overflow.errors.empty());
}

TEST_CASE("pivoted least-squares FIR is deterministic",
          "[signal][fir-design][least-squares][determinism]") {
    const std::vector<double> expected{0.1, -0.2, 0.6, -0.2, 0.1};
    const auto points = sampled_target(expected, LinearPhaseFirType::type_i_symmetric_odd);
    const auto first = design_fir_least_squares(
        points, {.tap_count = expected.size(), .type = LinearPhaseFirType::type_i_symmetric_odd});
    const auto second = design_fir_least_squares(
        points, {.tap_count = expected.size(), .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.coefficients == second.coefficients);
    REQUIRE(first.measured_amplitudes == second.measured_amplitudes);
    REQUIRE(first.qr_diagonal_condition_estimate == second.qr_diagonal_condition_estimate);
}

TEST_CASE("designed coefficients feed both double and intentionally narrowed FIR runtimes",
          "[signal][fir-design][consumer]") {
    const std::vector<double> expected{0.25, 0.5, 0.25};
    auto points = sampled_target(expected, LinearPhaseFirType::type_i_symmetric_odd);
    auto result = design_fir_least_squares(
        points, {.tap_count = 3u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(result);

    FirFilter64 double_filter;
    double_filter.set_coefficients(result.coefficients);
    REQUIRE_THAT(double_filter.process(1.0), WithinAbs(0.25, 1.0e-12));

    std::vector<float> narrowed(result.coefficients.begin(), result.coefficients.end());
    FirFilter float_filter;
    float_filter.set_coefficients(std::move(narrowed));
    REQUIRE_THAT(float_filter.process(1.0f), WithinAbs(0.25, 1.0e-6));
}
