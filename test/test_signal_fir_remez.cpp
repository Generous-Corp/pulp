// Equiripple (Parks-McClellan / Remez exchange) FIR design.
//
// The load-bearing oracle is the alternation theorem itself: a weighted-minimax
// optimum on r independent coefficients exhibits r+1 alternating extrema of
// equal weighted magnitude. That is checkable in-test from the returned filter
// alone, with no external reference. The negative control is the least-squares
// twin at the same tap count, which minimizes a different norm and must
// therefore show a strictly larger peak error.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/fir_design.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <span>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

/// Independent response: a direct DFT sum, not the library's evaluator.
std::complex<double> direct_dft_response_at(std::span<const double> impulse, double omega) {
    std::complex<double> sum{0.0, 0.0};
    for (std::size_t n = 0; n < impulse.size(); ++n)
        sum += impulse[n] * std::polar(1.0, -omega * static_cast<double>(n));
    return sum;
}

double direct_signed_zero_phase_amplitude(std::span<const double> coefficients,
                                          LinearPhaseFirType type, double omega) {
    const double delay = 0.5 * static_cast<double>(coefficients.size() - 1);
    const auto zero_phase =
        direct_dft_response_at(coefficients, omega) * std::polar(1.0, delay * omega);
    const bool antisymmetric = type == LinearPhaseFirType::type_iii_antisymmetric_odd ||
                               type == LinearPhaseFirType::type_iv_antisymmetric_even;
    return antisymmetric ? zero_phase.imag() : zero_phase.real();
}

struct BandPlan {
    std::vector<FirEquirippleBand> bands;
};

BandPlan lowpass_plan(double pass_edge, double stop_edge, double stop_weight = 1.0) {
    return BandPlan{{{0.0, pass_edge, 1.0, 1.0}, {stop_edge, kPi, 0.0, stop_weight}}};
}

/// Peak weighted deviation measured on a dense grid, independently of the
/// design's own reported numbers.
double measured_peak_weighted_error(std::span<const double> coefficients,
                                    LinearPhaseFirType type,
                                    std::span<const FirEquirippleBand> bands, int per_band = 400) {
    double peak = 0.0;
    for (const auto& band : bands) {
        for (int i = 0; i <= per_band; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(per_band);
            const double omega = band.lower_omega + t * (band.upper_omega - band.lower_omega);
            const double amplitude =
                direct_signed_zero_phase_amplitude(coefficients, type, omega);
            peak = std::max(peak, band.weight * std::abs(amplitude - band.amplitude));
        }
    }
    return peak;
}

} // namespace

TEST_CASE("equiripple design satisfies the alternation theorem", "[signal][fir][remez]") {
    const auto plan = lowpass_plan(0.20 * kPi, 0.30 * kPi);
    FirEquirippleOptions options;
    options.tap_count = 31;
    options.type = LinearPhaseFirType::type_i_symmetric_odd;

    const auto result = design_fir_equiripple(plan.bands, options);
    REQUIRE(result);
    REQUIRE(result.coefficients.size() == options.tap_count);

    // r independent coefficients demand exactly r+1 alternations.
    const std::size_t independent = options.tap_count / 2u + 1u;
    REQUIRE(result.extremal_frequencies.size() == independent + 1u);

    // Every alternation carries the same weighted magnitude, and the signs
    // alternate. Both are measured from the returned taps, not reported.
    double previous_sign = 0.0;
    for (const double omega : result.extremal_frequencies) {
        double desired = 0.0;
        double weight = 1.0;
        for (const auto& band : plan.bands) {
            if (omega >= band.lower_omega - 1e-12 && omega <= band.upper_omega + 1e-12) {
                desired = band.amplitude;
                weight = band.weight;
                break;
            }
        }
        const double amplitude =
            direct_signed_zero_phase_amplitude(result.coefficients, options.type, omega);
        const double signed_error = weight * (amplitude - desired);
        REQUIRE_THAT(std::abs(signed_error), WithinAbs(result.minimax_error, 1.0e-6));
        const double sign = signed_error > 0.0 ? 1.0 : -1.0;
        if (previous_sign != 0.0)
            REQUIRE(sign != previous_sign);
        previous_sign = sign;
    }

    // Equal weights must produce equal ripple in both bands.
    REQUIRE(result.band_ripples.size() == 2u);
    REQUIRE_THAT(result.band_ripples[0], WithinAbs(result.band_ripples[1], 1.0e-6));
}

TEST_CASE("equiripple beats least squares on peak error at the same order",
          "[signal][fir][remez]") {
    const auto plan = lowpass_plan(0.20 * kPi, 0.30 * kPi);
    FirEquirippleOptions options;
    options.tap_count = 31;
    const auto equiripple = design_fir_equiripple(plan.bands, options);
    REQUIRE(equiripple);

    // Same order, same spec, least-squares objective.
    std::vector<FirDesignPoint> points;
    for (int i = 0; i <= 400; ++i) {
        const double omega = kPi * static_cast<double>(i) / 400.0;
        if (omega <= plan.bands[0].upper_omega)
            points.push_back({omega, 1.0, 1.0});
        else if (omega >= plan.bands[1].lower_omega)
            points.push_back({omega, 0.0, 1.0});
    }
    FirLeastSquaresOptions ls_options;
    ls_options.tap_count = options.tap_count;
    ls_options.type = options.type;
    const auto least_squares = design_fir_least_squares(points, ls_options);
    REQUIRE(least_squares);

    const double equiripple_peak =
        measured_peak_weighted_error(equiripple.coefficients, options.type, plan.bands);
    const double least_squares_peak =
        measured_peak_weighted_error(least_squares.coefficients, options.type, plan.bands);

    INFO("equiripple peak " << equiripple_peak << " vs least-squares peak " << least_squares_peak);
    REQUIRE(equiripple_peak < least_squares_peak);
}

TEST_CASE("equiripple weights trade ripple between bands", "[signal][fir][remez]") {
    FirEquirippleOptions options;
    options.tap_count = 41;

    const auto even = lowpass_plan(0.20 * kPi, 0.30 * kPi, 1.0);
    const auto weighted = lowpass_plan(0.20 * kPi, 0.30 * kPi, 10.0);
    const auto even_result = design_fir_equiripple(even.bands, options);
    const auto weighted_result = design_fir_equiripple(weighted.bands, options);
    REQUIRE(even_result);
    REQUIRE(weighted_result);

    // A ten-times heavier stopband must buy a materially deeper stopband, paid
    // for by a larger passband ripple.
    REQUIRE(weighted_result.band_ripples[1] < even_result.band_ripples[1] * 0.5);
    REQUIRE(weighted_result.band_ripples[0] > even_result.band_ripples[0]);
    // The weighted errors are what the exchange equalizes.
    REQUIRE_THAT(10.0 * weighted_result.band_ripples[1],
                 WithinAbs(weighted_result.band_ripples[0], 1.0e-5));
}

TEST_CASE("equiripple fails closed on an infeasible ripple requirement",
          "[signal][fir][remez]") {
    const auto plan = lowpass_plan(0.20 * kPi, 0.30 * kPi);
    FirEquirippleOptions options;
    options.tap_count = 15;

    const auto unconstrained = design_fir_equiripple(plan.bands, options);
    REQUIRE(unconstrained);

    // Demand ripple an order below what this tap count can deliver.
    options.maximum_minimax_error = unconstrained.minimax_error * 0.1;
    const auto constrained = design_fir_equiripple(plan.bands, options);
    REQUIRE_FALSE(constrained);
    REQUIRE(constrained.status == FirDesignStatus::not_converged);
    REQUIRE(constrained.coefficients.empty());
}

TEST_CASE("equiripple rejects malformed specs without returning a filter",
          "[signal][fir][remez]") {
    FirEquirippleOptions options;
    options.tap_count = 31;

    auto expect_rejected = [&](std::vector<FirEquirippleBand> bands, FirDesignStatus expected,
                               const FirEquirippleOptions& used) {
        const auto result = design_fir_equiripple(bands, used);
        REQUIRE_FALSE(result);
        REQUIRE(result.status == expected);
        REQUIRE(result.coefficients.empty());
    };

    expect_rejected({}, FirDesignStatus::invalid_argument, options);
    // Overlapping bands.
    expect_rejected({{0.0, 0.5 * kPi, 1.0, 1.0}, {0.4 * kPi, kPi, 0.0, 1.0}},
                    FirDesignStatus::invalid_argument, options);
    // Descending band.
    expect_rejected({{0.5 * kPi, 0.2 * kPi, 1.0, 1.0}}, FirDesignStatus::invalid_argument, options);
    // Non-positive weight.
    expect_rejected({{0.0, 0.2 * kPi, 1.0, 0.0}}, FirDesignStatus::invalid_argument, options);
    // Beyond Nyquist.
    expect_rejected({{0.0, 1.5 * kPi, 1.0, 1.0}}, FirDesignStatus::invalid_argument, options);

    // Even tap count is not a Type I filter.
    FirEquirippleOptions parity = options;
    parity.tap_count = 30;
    expect_rejected({{0.0, 0.2 * kPi, 1.0, 1.0}, {0.3 * kPi, kPi, 0.0, 1.0}},
                    FirDesignStatus::invalid_argument, parity);

    // Above the admitted tap bound.
    FirEquirippleOptions oversized = options;
    oversized.tap_count = kMaximumFirDesignTapCount + 2u;
    expect_rejected({{0.0, 0.2 * kPi, 1.0, 1.0}, {0.3 * kPi, kPi, 0.0, 1.0}},
                    FirDesignStatus::unsupported_size, oversized);
}

TEST_CASE("equiripple is deterministic and preserves linear-phase symmetry",
          "[signal][fir][remez]") {
    const auto plan = lowpass_plan(0.22 * kPi, 0.34 * kPi);

    for (const auto type : {LinearPhaseFirType::type_i_symmetric_odd,
                            LinearPhaseFirType::type_ii_symmetric_even}) {
        FirEquirippleOptions options;
        options.type = type;
        options.tap_count = type == LinearPhaseFirType::type_i_symmetric_odd ? 33u : 34u;

        const auto first = design_fir_equiripple(plan.bands, options);
        const auto second = design_fir_equiripple(plan.bands, options);
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(first.coefficients == second.coefficients);
        REQUIRE(first.iterations == second.iterations);

        for (std::size_t i = 0; i < first.coefficients.size(); ++i)
            REQUIRE_THAT(first.coefficients[i],
                         WithinAbs(first.coefficients[first.coefficients.size() - 1u - i], 1e-15));
    }
}

TEST_CASE("equiripple honours a bounded iteration budget", "[signal][fir][remez]") {
    const auto plan = lowpass_plan(0.20 * kPi, 0.22 * kPi);
    FirEquirippleOptions options;
    options.tap_count = 81;
    options.maximum_iterations = 1; // far too few for this narrow transition

    const auto result = design_fir_equiripple(plan.bands, options);
    REQUIRE_FALSE(result);
    REQUIRE(result.status == FirDesignStatus::not_converged);
    REQUIRE(result.coefficients.empty());
    REQUIRE(result.iterations == 1u);
}
