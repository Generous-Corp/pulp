#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/fir_design.hpp>
#include <pulp/signal/fir_filter.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <numeric>
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

// Deliberately independent of FftT: a direct DFT prevents a shared FFT defect
// from making both the reconstruction and its magnitude oracle agree.
std::vector<double> direct_dft_magnitudes(std::span<const double> impulse, std::size_t fft_size) {
    std::vector<double> magnitudes(fft_size / 2u + 1u);
    for (std::size_t bin = 0u; bin < magnitudes.size(); ++bin) {
        const double omega = 2.0 * kPi * static_cast<double>(bin) / static_cast<double>(fft_size);
        magnitudes[bin] = std::abs(direct_dft_response_at(impulse, omega));
    }
    return magnitudes;
}

double front_half_energy(std::span<const double> values) {
    const std::size_t half = values.size() / 2u;
    double energy = 0.0;
    for (std::size_t index = 0u; index < half; ++index)
        energy += values[index] * values[index];
    return energy;
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
    REQUIRE_THAT(
        linear_phase_fir_amplitude(type_ii, LinearPhaseFirType::type_ii_symmetric_even, kPi),
        WithinAbs(0.0, 1.0e-14));
    REQUIRE_THAT(
        linear_phase_fir_amplitude(type_iii, LinearPhaseFirType::type_iii_antisymmetric_odd, 0.0),
        WithinAbs(0.0, 1.0e-14));
    REQUIRE_THAT(
        linear_phase_fir_amplitude(type_iii, LinearPhaseFirType::type_iii_antisymmetric_odd, kPi),
        WithinAbs(0.0, 1.0e-14));
    REQUIRE_THAT(
        linear_phase_fir_amplitude(type_iv, LinearPhaseFirType::type_iv_antisymmetric_even, 0.0),
        WithinAbs(0.0, 1.0e-14));
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

TEST_CASE("least-squares FIR rejects invalid parity, rank loss, conditioning, and budgets",
          "[signal][fir-design][least-squares][fault]") {
    const std::vector<FirDesignPoint> healthy{
        {0.0, 1.0, 1.0}, {0.4, 0.8, 1.0}, {1.2, 0.2, 1.0}, {kPi, 0.0, 1.0}};
    auto bad_parity = design_fir_least_squares(
        healthy, {.tap_count = 4u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(bad_parity.status == FirDesignStatus::invalid_argument);

    const std::vector<FirDesignPoint> duplicate(4u, FirDesignPoint{0.3, 1.0, 1.0});
    auto rank_lost = design_fir_least_squares(
        duplicate, {.tap_count = 5u, .type = LinearPhaseFirType::type_i_symmetric_odd});
    REQUIRE(rank_lost.status == FirDesignStatus::rank_deficient);
    REQUIRE(rank_lost.numerical_rank < 3u);

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

TEST_CASE("minimum-phase reconstruction recovers an untruncated magnitude and stable zero",
          "[signal][fir-design][minimum-phase]") {
    constexpr std::size_t fft_size = 64u;
    const std::vector<double> known_minimum_phase{1.0, -0.5};
    const auto target = direct_dft_magnitudes(known_minimum_phase, fft_size);
    const auto result = reconstruct_minimum_phase_fir(target);
    REQUIRE(result.status == FirDesignStatus::success);
    REQUIRE(result.fft_size == fft_size);
    REQUIRE(result.coefficients.size() == fft_size);
    REQUIRE(result.maximum_absolute_error < 2.0e-12);
    REQUIRE(result.rms_error < 1.0e-12);
    REQUIRE_THAT(result.coefficients[0], WithinAbs(1.0, 2.0e-12));
    REQUIRE_THAT(result.coefficients[1], WithinAbs(-0.5, 2.0e-12));
    const double recovered_zero = -result.coefficients[1] / result.coefficients[0];
    REQUIRE_THAT(recovered_zero, WithinAbs(0.5, 2.0e-12));
    REQUIRE(std::abs(recovered_zero) < 1.0);
    const auto independent_returned = direct_dft_magnitudes(result.coefficients, fft_size);
    for (std::size_t bin = 0u; bin < target.size(); ++bin) {
        REQUIRE_THAT(independent_returned[bin], WithinAbs(target[bin], 2.0e-12));
        REQUIRE_THAT(result.measured_magnitudes[bin],
                     WithinAbs(independent_returned[bin], 2.0e-12));
    }
}

TEST_CASE("minimum-phase reconstruction front-loads energy independently of magnitude fit",
          "[signal][fir-design][minimum-phase]") {
    constexpr std::size_t fft_size = 128u;
    const std::vector<double> symmetric{0.02, 0.08, 0.2, 0.4, 0.6, 0.4, 0.2, 0.08, 0.02};
    const auto target = direct_dft_magnitudes(symmetric, fft_size);
    const auto result = reconstruct_minimum_phase_fir(target);
    REQUIRE(result);
    REQUIRE(result.maximum_absolute_error < 1.0e-9);
    const double total_energy = std::inner_product(
        result.coefficients.begin(), result.coefficients.end(), result.coefficients.begin(), 0.0);
    REQUIRE(front_half_energy(result.coefficients) > 0.99 * total_energy);
}

TEST_CASE("minimum-phase floor and truncation are observable in measured error",
          "[signal][fir-design][minimum-phase]") {
    const std::vector<double> zeros(33u, 0.0);
    const auto floored = reconstruct_minimum_phase_fir(
        zeros, {.coefficient_count = 64u, .log_magnitude_floor = 1.0e-6});
    REQUIRE(floored);
    REQUIRE_THAT(floored.coefficients[0], WithinAbs(1.0e-6, 1.0e-15));
    REQUIRE(floored.maximum_absolute_error < 1.0e-14);

    const auto target = direct_dft_magnitudes(std::vector<double>{1.0, -0.8}, 64u);
    const auto truncated = reconstruct_minimum_phase_fir(target, {.coefficient_count = 1u});
    REQUIRE(truncated);
    REQUIRE(truncated.coefficients.size() == 1u);
    REQUIRE(truncated.maximum_absolute_error > 0.7);
    REQUIRE(truncated.rms_error > 0.4);
    REQUIRE_THAT(truncated.errors.front(), WithinAbs(0.8, 2.0e-8));
}

TEST_CASE("minimum-phase reconstruction rejects invalid geometry, values, and budgets",
          "[signal][fir-design][minimum-phase][fault]") {
    REQUIRE(reconstruct_minimum_phase_fir(std::vector<double>{1.0}).status ==
            FirDesignStatus::invalid_argument);
    REQUIRE(reconstruct_minimum_phase_fir(std::vector<double>{1.0, 1.0, 1.0, 1.0}).status ==
            FirDesignStatus::invalid_argument); // Implied N=6 is not radix-2.
    REQUIRE(reconstruct_minimum_phase_fir(std::vector<double>(33u, 1.0), {.coefficient_count = 65u})
                .status == FirDesignStatus::invalid_argument);
    auto nonfinite = std::vector<double>(33u, 1.0);
    nonfinite[4] = std::numeric_limits<double>::infinity();
    REQUIRE(reconstruct_minimum_phase_fir(nonfinite).status == FirDesignStatus::invalid_argument);
    REQUIRE(reconstruct_minimum_phase_fir(std::vector<double>(33u, 1.0),
                                          {.coefficient_count = 64u,
                                           .log_magnitude_floor = 1.0e-12,
                                           .maximum_workspace_bytes = 8u})
                .status == FirDesignStatus::workspace_limit_exceeded);

    std::vector<double> extreme(33u, std::numeric_limits<double>::max());
    for (std::size_t bin = 1u; bin < extreme.size(); bin += 2u)
        extreme[bin] = std::numeric_limits<double>::min();
    const auto overflow = reconstruct_minimum_phase_fir(extreme);
    REQUIRE(overflow.status == FirDesignStatus::numerical_failure);
    REQUIRE(overflow.fft_size == 0u);
    REQUIRE(overflow.coefficients.empty());
    REQUIRE(overflow.measured_magnitudes.empty());
    REQUIRE(overflow.errors.empty());
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
