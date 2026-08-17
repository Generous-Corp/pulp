#pragma once

/// @file fir_design.hpp
/// Offline FIR design from sampled frequency-domain targets.

#include <pulp/signal/checked_allocation.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

namespace pulp::signal {

inline constexpr std::uint64_t kDefaultFirDesignWorkspaceBytes = 256u * 1024u * 1024u;
inline constexpr std::size_t kMaximumFirDesignTapCount = 1023u;
inline constexpr std::size_t kMaximumFirDesignPointCount = 65536u;
/// Largest admitted radix-2 FFT size for minimum-phase reconstruction. This
/// mirrors the point-count bound: the one-sided input is a sampled grid too.
inline constexpr std::size_t kMaximumMinimumPhaseFirSize = 65536u;

enum class FirDesignStatus {
    success,
    invalid_argument,
    unsupported_size,
    workspace_limit_exceeded,
    allocation_failure,
    rank_deficient,
    ill_conditioned,
    numerical_failure,
};

enum class LinearPhaseFirType {
    type_i_symmetric_odd,
    type_ii_symmetric_even,
    type_iii_antisymmetric_odd,
    type_iv_antisymmetric_even,
};

struct FirDesignPoint {
    /// Angular frequency in radians/sample, in the closed interval [0, pi].
    double omega = 0.0;
    /// Signed zero-phase amplitude. For Types III/IV this is the coefficient
    /// of +j after removing the filter's linear delay.
    double amplitude = 0.0;
    /// Positive least-squares weight. The objective is sum(weight * error^2).
    double weight = 1.0;
};

struct FirLeastSquaresOptions {
    std::size_t tap_count = 0;
    LinearPhaseFirType type = LinearPhaseFirType::type_i_symmetric_odd;
    double rank_tolerance = 1.0e-12;
    double maximum_diagonal_condition_estimate = 1.0e10;
    std::uint64_t maximum_workspace_bytes = kDefaultFirDesignWorkspaceBytes;
};

struct FirLeastSquaresResult {
    FirDesignStatus status = FirDesignStatus::invalid_argument;
    std::vector<double> coefficients;
    std::vector<double> measured_amplitudes;
    std::vector<double> errors;
    double weighted_rms_error = 0.0;
    double maximum_absolute_error = 0.0;
    std::size_t numerical_rank = 0;
    /// Ratio of largest to smallest accepted diagonal of pivoted R. This is a
    /// deterministic warning metric, not the 2-norm condition number of A.
    double qr_diagonal_condition_estimate = 0.0;

    explicit operator bool() const noexcept {
        return status == FirDesignStatus::success;
    }
};

struct MinimumPhaseFirOptions {
    /// Zero means retain the complete circular impulse of fft_size samples.
    std::size_t coefficient_count = 0;
    /// Every magnitude below this positive floor, including zero, is replaced
    /// before log(). This bounds the cepstrum of exact spectral nulls.
    double log_magnitude_floor = 1.0e-12;
    std::uint64_t maximum_workspace_bytes = kDefaultFirDesignWorkspaceBytes;
};

struct MinimumPhaseFirResult {
    FirDesignStatus status = FirDesignStatus::invalid_argument;
    std::size_t fft_size = 0;
    std::vector<double> coefficients;
    std::vector<double> measured_magnitudes;
    std::vector<double> errors;
    double rms_error = 0.0;
    double maximum_absolute_error = 0.0;

    explicit operator bool() const noexcept {
        return status == FirDesignStatus::success;
    }
};

namespace fir_design_detail {

inline constexpr double pi = 3.141592653589793238462643383279502884;

inline bool is_forced_zero_endpoint(LinearPhaseFirType type, double omega) noexcept {
    const bool at_dc = omega == 0.0;
    const bool at_nyquist = omega == pi;
    return (type == LinearPhaseFirType::type_ii_symmetric_even && at_nyquist) ||
           (type == LinearPhaseFirType::type_iii_antisymmetric_odd &&
            (at_dc || at_nyquist)) ||
           (type == LinearPhaseFirType::type_iv_antisymmetric_even && at_dc);
}

} // namespace fir_design_detail

inline std::complex<double> fir_response(std::span<const double> coefficients,
                                         double omega) noexcept {
    std::complex<double> response{};
    const std::complex<double> step = std::polar(1.0, -omega);
    std::complex<double> phase{1.0, 0.0};
    for (double coefficient : coefficients) {
        response += coefficient * phase;
        phase *= step;
    }
    return response;
}

/// Signed amplitude after removing the exact linear delay of a Type I-IV FIR.
/// Symmetric filters use the real component. Antisymmetric filters use the +j
/// component, matching FirDesignPoint::amplitude.
inline double linear_phase_fir_amplitude(std::span<const double> coefficients,
                                         LinearPhaseFirType type, double omega) noexcept {
    if (coefficients.empty())
        return 0.0;
    if (fir_design_detail::is_forced_zero_endpoint(type, omega))
        return 0.0;
    const double delay = 0.5 * static_cast<double>(coefficients.size() - 1u);
    const auto zero_phase = fir_response(coefficients, omega) * std::polar(1.0, delay * omega);
    const bool antisymmetric = type == LinearPhaseFirType::type_iii_antisymmetric_odd ||
                               type == LinearPhaseFirType::type_iv_antisymmetric_even;
    return antisymmetric ? zero_phase.imag() : zero_phase.real();
}

namespace fir_design_detail {

inline bool valid_type_length(LinearPhaseFirType type, std::size_t count) noexcept {
    if (count == 0u)
        return false;
    switch (type) {
    case LinearPhaseFirType::type_i_symmetric_odd:
        return (count & 1u) != 0u;
    case LinearPhaseFirType::type_ii_symmetric_even:
        return (count & 1u) == 0u;
    case LinearPhaseFirType::type_iii_antisymmetric_odd:
        return count >= 3u && (count & 1u) != 0u;
    case LinearPhaseFirType::type_iv_antisymmetric_even:
        return (count & 1u) == 0u;
    }
    return false;
}

inline std::size_t independent_coefficient_count(LinearPhaseFirType type,
                                                 std::size_t count) noexcept {
    return type == LinearPhaseFirType::type_i_symmetric_odd ? count / 2u + 1u : count / 2u;
}

inline double basis_value(LinearPhaseFirType type, std::size_t column, double omega) noexcept {
    if (is_forced_zero_endpoint(type, omega))
        return 0.0;
    switch (type) {
    case LinearPhaseFirType::type_i_symmetric_odd:
        return column == 0u ? 1.0 : 2.0 * std::cos(static_cast<double>(column) * omega);
    case LinearPhaseFirType::type_ii_symmetric_even:
        return 2.0 * std::cos((static_cast<double>(column) + 0.5) * omega);
    case LinearPhaseFirType::type_iii_antisymmetric_odd:
        return 2.0 * std::sin((static_cast<double>(column) + 1.0) * omega);
    case LinearPhaseFirType::type_iv_antisymmetric_even:
        return 2.0 * std::sin((static_cast<double>(column) + 0.5) * omega);
    }
    return 0.0;
}

inline void clear_payload(FirLeastSquaresResult& result) noexcept {
    result.coefficients.clear();
    result.measured_amplitudes.clear();
    result.errors.clear();
    result.weighted_rms_error = 0.0;
    result.maximum_absolute_error = 0.0;
}

inline void clear_payload(MinimumPhaseFirResult& result) noexcept {
    result.fft_size = 0u;
    result.coefficients.clear();
    result.measured_magnitudes.clear();
    result.errors.clear();
    result.rms_error = 0.0;
    result.maximum_absolute_error = 0.0;
}

inline bool is_power_of_two(std::size_t value) noexcept {
    return value >= 2u && (value & (value - 1u)) == 0u;
}

inline bool admit_minimum_phase_workspace(std::size_t fft_size, std::size_t bins,
                                          std::size_t coefficients, std::uint64_t limit) noexcept {
    if (limit == 0u)
        return false;
    CheckedRetainedByteCharge charge(limit);
    // One complex work spectrum, FftT's retained twiddles, coefficients,
    // measured bins, and errors. No hidden full-spectrum copy is retained.
    if (!charge.add<std::complex<double>>(fft_size) ||
        !charge.add<std::complex<double>>(fft_size / 2u) || !charge.add<double>(coefficients) ||
        !charge.add<double>(bins) || !charge.add<double>(bins))
        return false;
    return charge.total() <= limit;
}

inline bool admit_least_squares_workspace(std::size_t rows, std::size_t columns, std::size_t taps,
                                          std::uint64_t limit) noexcept {
    std::uint64_t matrix_elements = 0;
    if (limit == 0u ||
        !checked_capacity_product(
            rows, columns, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()),
            matrix_elements))
        return false;
    CheckedRetainedByteCharge charge(limit);
    if (!charge.add<double>(matrix_elements) || !charge.add<double>(rows) ||
        !charge.add<std::size_t>(columns) || !charge.add<double>(columns) ||
        !charge.add<double>(columns) || !charge.add<double>(columns) ||
        !charge.add<double>(columns) || !charge.add<double>(taps) || !charge.add<double>(rows) ||
        !charge.add<double>(rows))
        return false;
    return charge.total() <= limit;
}

inline void reconstruct_coefficients(LinearPhaseFirType type, std::span<const double> independent,
                                     std::span<double> coefficients) noexcept {
    const std::size_t count = coefficients.size();
    const std::size_t half = count / 2u;
    if (type == LinearPhaseFirType::type_i_symmetric_odd) {
        coefficients[half] = independent[0];
        for (std::size_t k = 1u; k < independent.size(); ++k)
            coefficients[half - k] = coefficients[half + k] = independent[k];
        return;
    }
    const bool antisymmetric = type == LinearPhaseFirType::type_iii_antisymmetric_odd ||
                               type == LinearPhaseFirType::type_iv_antisymmetric_even;
    if (type == LinearPhaseFirType::type_iii_antisymmetric_odd)
        coefficients[half] = 0.0;
    for (std::size_t k = 0u; k < independent.size(); ++k) {
        const std::size_t offset =
            type == LinearPhaseFirType::type_iii_antisymmetric_odd ? k + 1u : k;
        const std::size_t left = half - 1u - k;
        const std::size_t right =
            type == LinearPhaseFirType::type_iii_antisymmetric_odd ? half + offset : half + k;
        coefficients[left] = independent[k];
        coefficients[right] = antisymmetric ? -independent[k] : independent[k];
    }
}

} // namespace fir_design_detail

/// Weighted least-squares design of a real Type I, II, III, or IV linear-phase FIR.
///
/// This function allocates and performs O(grid*taps^2) work. It is for design
/// and control threads, never an audio callback. Frequencies may be in any
/// order. A deterministic column-pivoted Householder QR rejects rank loss and
/// excessive R-diagonal spread instead of solving unstable normal equations.
/// Targets are absolute signed zero-phase amplitudes: there is no implicit DC,
/// peak, or energy normalization.
inline FirLeastSquaresResult design_fir_least_squares(std::span<const FirDesignPoint> points,
                                                      const FirLeastSquaresOptions& options) {
    using namespace fir_design_detail;
    FirLeastSquaresResult result;
    if (options.tap_count > kMaximumFirDesignTapCount ||
        points.size() > kMaximumFirDesignPointCount) {
        result.status = FirDesignStatus::unsupported_size;
        return result;
    }
    if (!valid_type_length(options.type, options.tap_count) || points.empty() ||
        !(options.rank_tolerance > 0.0) || options.rank_tolerance > 1.0 ||
        !std::isfinite(options.rank_tolerance) ||
        !(options.maximum_diagonal_condition_estimate >= 1.0) ||
        !std::isfinite(options.maximum_diagonal_condition_estimate))
        return result;

    const std::size_t rows = points.size();
    const std::size_t columns = independent_coefficient_count(options.type, options.tap_count);
    double maximum_weight = 0.0;
    for (const auto& point : points) {
        if (!std::isfinite(point.omega) || point.omega < 0.0 || point.omega > pi ||
            !std::isfinite(point.amplitude) || !(point.weight > 0.0) ||
            !std::isfinite(point.weight))
            return result;
        maximum_weight = std::max(maximum_weight, point.weight);
    }
    if (rows < columns) {
        result.status = FirDesignStatus::rank_deficient;
        return result;
    }
    const double maximum_weight_sqrt = std::sqrt(maximum_weight);
    if (!admit_least_squares_workspace(rows, columns, options.tap_count,
                                       options.maximum_workspace_bytes)) {
        result.status = FirDesignStatus::workspace_limit_exceeded;
        return result;
    }

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        std::vector<double> matrix(rows * columns);
        std::vector<double> rhs(rows);
        std::vector<std::size_t> permutation(columns);
        std::vector<double> solution(columns);
        for (std::size_t row = 0u; row < rows; ++row) {
            // A common positive scale does not change the minimizer. Dividing
            // the square roots separately preserves weight ratios that remain
            // representable even when the ratio itself would underflow.
            const double scale = std::sqrt(points[row].weight) / maximum_weight_sqrt;
            rhs[row] = scale * points[row].amplitude;
            if (!std::isfinite(rhs[row])) {
                result.status = FirDesignStatus::numerical_failure;
                return result;
            }
            for (std::size_t column = 0u; column < columns; ++column) {
                matrix[row * columns + column] =
                    scale * basis_value(options.type, column, points[row].omega);
                if (!std::isfinite(matrix[row * columns + column])) {
                    result.status = FirDesignStatus::numerical_failure;
                    return result;
                }
            }
        }
        std::iota(permutation.begin(), permutation.end(), std::size_t{0});

        double leading_norm = 0.0;
        for (std::size_t column = 0u; column < columns; ++column) {
            double norm = 0.0;
            for (std::size_t row = 0u; row < rows; ++row)
                norm = std::hypot(norm, matrix[row * columns + column]);
            leading_norm = std::max(leading_norm, norm);
        }
        if (!std::isfinite(leading_norm)) {
            result.status = FirDesignStatus::numerical_failure;
            return result;
        }
        if (!(leading_norm > 0.0)) {
            result.status = FirDesignStatus::rank_deficient;
            return result;
        }

        const double rank_threshold = options.rank_tolerance * leading_norm;
        double minimum_diagonal = std::numeric_limits<double>::infinity();
        double maximum_diagonal = 0.0;
        for (std::size_t step = 0u; step < columns; ++step) {
            std::size_t pivot = step;
            double pivot_norm = -1.0;
            for (std::size_t column = step; column < columns; ++column) {
                double norm = 0.0;
                for (std::size_t row = step; row < rows; ++row)
                    norm = std::hypot(norm, matrix[row * columns + column]);
                if (norm > pivot_norm) { // Strict comparison preserves the first tie.
                    pivot_norm = norm;
                    pivot = column;
                }
            }
            if (!std::isfinite(pivot_norm)) {
                result.status = FirDesignStatus::numerical_failure;
                result.numerical_rank = step;
                return result;
            }
            if (!(pivot_norm > rank_threshold)) {
                result.status = FirDesignStatus::rank_deficient;
                result.numerical_rank = step;
                return result;
            }
            if (pivot != step) {
                for (std::size_t row = 0u; row < rows; ++row)
                    std::swap(matrix[row * columns + step], matrix[row * columns + pivot]);
                std::swap(permutation[step], permutation[pivot]);
            }

            const double x0 = matrix[step * columns + step];
            const double alpha = x0 >= 0.0 ? -pivot_norm : pivot_norm;
            matrix[step * columns + step] = x0 - alpha;
            double vector_norm_squared = 0.0;
            for (std::size_t row = step; row < rows; ++row) {
                const double value = matrix[row * columns + step];
                vector_norm_squared += value * value;
            }
            if (!(vector_norm_squared > 0.0) || !std::isfinite(vector_norm_squared)) {
                result.status = FirDesignStatus::numerical_failure;
                clear_payload(result);
                return result;
            }
            const double beta = 2.0 / vector_norm_squared;
            for (std::size_t column = step + 1u; column < columns; ++column) {
                double dot = 0.0;
                for (std::size_t row = step; row < rows; ++row)
                    dot += matrix[row * columns + step] * matrix[row * columns + column];
                const double factor = beta * dot;
                for (std::size_t row = step; row < rows; ++row)
                    matrix[row * columns + column] -= factor * matrix[row * columns + step];
            }
            double rhs_dot = 0.0;
            for (std::size_t row = step; row < rows; ++row)
                rhs_dot += matrix[row * columns + step] * rhs[row];
            const double rhs_factor = beta * rhs_dot;
            for (std::size_t row = step; row < rows; ++row)
                rhs[row] -= rhs_factor * matrix[row * columns + step];
            matrix[step * columns + step] = alpha;
            for (std::size_t row = step + 1u; row < rows; ++row)
                matrix[row * columns + step] = 0.0;

            const double diagonal = std::abs(alpha);
            minimum_diagonal = std::min(minimum_diagonal, diagonal);
            maximum_diagonal = std::max(maximum_diagonal, diagonal);
            result.numerical_rank = step + 1u;
        }

        result.qr_diagonal_condition_estimate = maximum_diagonal / minimum_diagonal;
        if (!std::isfinite(result.qr_diagonal_condition_estimate)) {
            result.status = FirDesignStatus::numerical_failure;
            return result;
        }
        if (result.qr_diagonal_condition_estimate > options.maximum_diagonal_condition_estimate) {
            result.status = FirDesignStatus::ill_conditioned;
            return result;
        }

        for (std::size_t reverse = columns; reverse-- > 0u;) {
            double value = rhs[reverse];
            for (std::size_t column = reverse + 1u; column < columns; ++column)
                value -= matrix[reverse * columns + column] * solution[column];
            solution[reverse] = value / matrix[reverse * columns + reverse];
            if (!std::isfinite(solution[reverse])) {
                result.status = FirDesignStatus::numerical_failure;
                clear_payload(result);
                return result;
            }
        }

        // Semi-normal refinement recovers small projections that can be lost
        // when a large target component is nearly orthogonal to the design
        // space. Limit it to systems where squaring the diagonal spread still
        // leaves ample precision; the primary QR result handles the rest.
        const double refinement_condition_limit =
            1.0 / std::sqrt(std::sqrt(std::numeric_limits<double>::epsilon()));
        if (result.qr_diagonal_condition_estimate <= refinement_condition_limit) {
            std::vector<double> correction(columns, 0.0);
            std::vector<double> gradient(columns, 0.0);
            for (std::size_t row = 0u; row < rows; ++row) {
                const double scale = std::sqrt(points[row].weight) / maximum_weight_sqrt;
                double predicted = 0.0;
                for (std::size_t column = 0u; column < columns; ++column) {
                    const double value =
                        scale * basis_value(options.type, permutation[column], points[row].omega);
                    predicted = std::fma(value, solution[column], predicted);
                }
                const double residual = scale * points[row].amplitude - predicted;
                if (!std::isfinite(residual)) {
                    result.status = FirDesignStatus::numerical_failure;
                    clear_payload(result);
                    return result;
                }
                for (std::size_t column = 0u; column < columns; ++column) {
                    const double value =
                        scale * basis_value(options.type, permutation[column], points[row].omega);
                    gradient[column] = std::fma(value, residual, gradient[column]);
                }
            }
            for (std::size_t column = 0u; column < columns; ++column) {
                double value = gradient[column];
                if (!std::isfinite(value)) {
                    result.status = FirDesignStatus::numerical_failure;
                    clear_payload(result);
                    return result;
                }
                for (std::size_t row = 0u; row < column; ++row)
                    value -= matrix[row * columns + column] * correction[row];
                correction[column] = value / matrix[column * columns + column];
                if (!std::isfinite(correction[column])) {
                    result.status = FirDesignStatus::numerical_failure;
                    clear_payload(result);
                    return result;
                }
            }
            for (std::size_t reverse = columns; reverse-- > 0u;) {
                double value = correction[reverse];
                for (std::size_t column = reverse + 1u; column < columns; ++column)
                    value -= matrix[reverse * columns + column] * correction[column];
                correction[reverse] = value / matrix[reverse * columns + reverse];
                if (!std::isfinite(correction[reverse])) {
                    result.status = FirDesignStatus::numerical_failure;
                    clear_payload(result);
                    return result;
                }
            }
            double linear_gain = 0.0;
            double correction_energy = 0.0;
            for (std::size_t row = 0u; row < columns; ++row) {
                linear_gain = std::fma(correction[row], gradient[row], linear_gain);
                double transformed = 0.0;
                for (std::size_t column = row; column < columns; ++column)
                    transformed =
                        std::fma(matrix[row * columns + column], correction[column], transformed);
                correction_energy = std::fma(transformed, transformed, correction_energy);
            }
            const double predicted_reduction = 2.0 * linear_gain - correction_energy;
            if (std::isfinite(predicted_reduction) && predicted_reduction > 0.0) {
                bool finite_candidate = true;
                for (std::size_t column = 0u; column < columns; ++column) {
                    gradient[column] = solution[column] + correction[column];
                    finite_candidate = finite_candidate && std::isfinite(gradient[column]);
                }
                if (finite_candidate)
                    solution = gradient;
            }
        }
        std::vector<double> unpermuted(columns);
        for (std::size_t column = 0u; column < columns; ++column)
            unpermuted[permutation[column]] = solution[column];

        result.coefficients.assign(options.tap_count, 0.0);
        reconstruct_coefficients(options.type, unpermuted, result.coefficients);
        result.measured_amplitudes.resize(rows);
        result.errors.resize(rows);
        double weighted_squared_error = 0.0;
        double weight_sum = 0.0;
        for (std::size_t row = 0u; row < rows; ++row) {
            const double measured =
                linear_phase_fir_amplitude(result.coefficients, options.type, points[row].omega);
            const double error = measured - points[row].amplitude;
            if (!std::isfinite(measured) || !std::isfinite(error)) {
                result.status = FirDesignStatus::numerical_failure;
                clear_payload(result);
                return result;
            }
            result.measured_amplitudes[row] = measured;
            result.errors[row] = error;
            result.maximum_absolute_error =
                std::max(result.maximum_absolute_error, std::abs(error));
            const double scale = std::sqrt(points[row].weight) / maximum_weight_sqrt;
            const double scaled_error = scale * error;
            if (!std::isfinite(scaled_error)) {
                result.status = FirDesignStatus::numerical_failure;
                clear_payload(result);
                return result;
            }
            weighted_squared_error += scaled_error * scaled_error;
            weight_sum += scale * scale;
        }
        result.weighted_rms_error = std::sqrt(weighted_squared_error / weight_sum);
        if (!std::isfinite(result.weighted_rms_error)) {
            result.status = FirDesignStatus::numerical_failure;
            clear_payload(result);
            return result;
        }
        result.status = FirDesignStatus::success;
        return result;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (const std::bad_alloc&) {
        clear_payload(result);
        result.status = FirDesignStatus::allocation_failure;
        return result;
    } catch (const std::length_error&) {
        clear_payload(result);
        result.status = FirDesignStatus::allocation_failure;
        return result;
    }
#endif
}

/// Reconstruct a causal minimum-phase FIR from N/2+1 nonnegative magnitude bins.
///
/// Input bins cover DC through Nyquist for the implied even FFT size
/// N=2*(bins-1). Exact zeros are floored before the logarithm. Retaining fewer
/// than N coefficients truncates the circular impulse and therefore changes the
/// requested magnitude; measured_magnitudes and errors always report that
/// post-truncation result. This is allocation-allowed offline or control work,
/// never an audio callback. The result is minimum phase within the finite,
/// floored FFT geometry; it is not a linear-phase design.
inline MinimumPhaseFirResult
reconstruct_minimum_phase_fir(std::span<const double> one_sided_magnitudes,
                              const MinimumPhaseFirOptions& options = {}) {
    using namespace fir_design_detail;
    MinimumPhaseFirResult result;
    if (one_sided_magnitudes.size() < 2u ||
        one_sided_magnitudes.size() - 1u > std::numeric_limits<std::size_t>::max() / 2u ||
        !(options.log_magnitude_floor > 0.0) || !std::isfinite(options.log_magnitude_floor))
        return result;
    const std::size_t fft_size = 2u * (one_sided_magnitudes.size() - 1u);
    const std::size_t coefficient_count =
        options.coefficient_count == 0u ? fft_size : options.coefficient_count;
    if (fft_size > kMaximumMinimumPhaseFirSize ||
        fft_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        result.status = FirDesignStatus::unsupported_size;
        return result;
    }
    if (!is_power_of_two(fft_size) || coefficient_count == 0u || coefficient_count > fft_size)
        return result;
    for (double magnitude : one_sided_magnitudes) {
        if (!(magnitude >= 0.0) || !std::isfinite(magnitude))
            return result;
    }
    if (!admit_minimum_phase_workspace(fft_size, one_sided_magnitudes.size(), coefficient_count,
                                       options.maximum_workspace_bytes)) {
        result.status = FirDesignStatus::workspace_limit_exceeded;
        return result;
    }

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        std::vector<std::complex<double>> spectrum(fft_size);
        for (std::size_t bin = 0u; bin < one_sided_magnitudes.size(); ++bin) {
            const double log_magnitude =
                std::log(std::max(one_sided_magnitudes[bin], options.log_magnitude_floor));
            if (!std::isfinite(log_magnitude)) {
                result.status = FirDesignStatus::numerical_failure;
                clear_payload(result);
                return result;
            }
            spectrum[bin] = {log_magnitude, 0.0};
        }
        for (std::size_t bin = one_sided_magnitudes.size(); bin < fft_size; ++bin)
            spectrum[bin] = spectrum[fft_size - bin];

        FftT<double> fft(static_cast<int>(fft_size));
        fft.inverse(spectrum.data());
        for (const auto value : spectrum) {
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                result.status = FirDesignStatus::numerical_failure;
                clear_payload(result);
                return result;
            }
        }
        for (std::size_t quefrency = 1u; quefrency < fft_size / 2u; ++quefrency)
            spectrum[quefrency] *= 2.0;
        for (std::size_t quefrency = fft_size / 2u + 1u; quefrency < fft_size; ++quefrency)
            spectrum[quefrency] = {};
        fft.forward(spectrum.data());
        for (auto& value : spectrum) {
            value = std::exp(value);
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                result.status = FirDesignStatus::numerical_failure;
                clear_payload(result);
                return result;
            }
        }
        fft.inverse(spectrum.data());

        result.fft_size = fft_size;
        result.coefficients.resize(coefficient_count);
        for (std::size_t index = 0u; index < coefficient_count; ++index) {
            const double value = spectrum[index].real();
            if (!std::isfinite(value)) {
                result.status = FirDesignStatus::numerical_failure;
                clear_payload(result);
                return result;
            }
            result.coefficients[index] = value;
        }

        std::fill(spectrum.begin(), spectrum.end(), std::complex<double>{});
        for (std::size_t index = 0u; index < coefficient_count; ++index)
            spectrum[index] = {result.coefficients[index], 0.0};
        fft.forward(spectrum.data());
        result.measured_magnitudes.resize(one_sided_magnitudes.size());
        result.errors.resize(one_sided_magnitudes.size());
        double squared_error = 0.0;
        for (std::size_t bin = 0u; bin < one_sided_magnitudes.size(); ++bin) {
            const double measured = std::abs(spectrum[bin]);
            const double target = std::max(one_sided_magnitudes[bin], options.log_magnitude_floor);
            const double error = measured - target;
            if (!std::isfinite(measured) || !std::isfinite(error)) {
                result.status = FirDesignStatus::numerical_failure;
                clear_payload(result);
                return result;
            }
            result.measured_magnitudes[bin] = measured;
            result.errors[bin] = error;
            result.maximum_absolute_error =
                std::max(result.maximum_absolute_error, std::abs(error));
            squared_error += error * error;
        }
        result.rms_error =
            std::sqrt(squared_error / static_cast<double>(one_sided_magnitudes.size()));
        if (!std::isfinite(result.rms_error)) {
            result.status = FirDesignStatus::numerical_failure;
            clear_payload(result);
            return result;
        }
        result.status = FirDesignStatus::success;
        return result;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (const std::bad_alloc&) {
        clear_payload(result);
        result.status = FirDesignStatus::allocation_failure;
        return result;
    } catch (const std::length_error&) {
        clear_payload(result);
        result.status = FirDesignStatus::allocation_failure;
        return result;
    }
#endif
}

} // namespace pulp::signal
