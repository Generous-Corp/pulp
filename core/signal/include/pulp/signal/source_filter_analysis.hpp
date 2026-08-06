#pragma once

/// @file source_filter_analysis.hpp
/// Prepared, allocation-free spectral-envelope and linear-prediction analysis.
///
/// These analyzers are mutable single-owner objects intended for control-thread
/// or offline work. `prepare()` allocates; analysis calls retain no borrowed
/// spans and allocate nothing. They are not safe for concurrent use.

#include <pulp/signal/checked_allocation.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::signal {

enum class SourceFilterAnalysisStatus : std::uint8_t {
    Ok,
    NotPrepared,
    InvalidFftSize,
    InvalidOrder,
    InvalidIterationCount,
    InvalidTolerance,
    InvalidInputSize,
    InvalidOutputSize,
    InvalidWorkspaceSize,
    AllocationFailure,
    NonFiniteInput,
    NumericalOverflow,
    DegenerateInput,
    RankDeficient,
    UnstableModel,
    PredictionErrorOverflow,
    InvalidSampleRate,
    InvalidGain,
    ResponseSingularity,
};

inline constexpr int kSourceFilterMinimumFftSize = 256;
inline constexpr int kSourceFilterMaximumFftSize = 1 << 20;
inline constexpr int kSourceFilterMaximumLpcOrder = 256;
inline constexpr int kSourceFilterMaximumEnvelopeIterations = 1024;

inline bool source_filter_fft_size_valid(int fft_size) noexcept {
    return fft_size >= kSourceFilterMinimumFftSize && fft_size <= kSourceFilterMaximumFftSize &&
           (fft_size & (fft_size - 1)) == 0;
}

template <typename SampleType> struct CepstralEnvelopeConfigT {
    static_assert(std::is_floating_point_v<SampleType>);

    int fft_size = 2048;
    int order = 128;
    int true_envelope_iterations = 3;
    /// Maximum remaining positive log-magnitude residual. Zero disables early
    /// convergence and always executes the configured number of passes.
    SampleType convergence_tolerance = SampleType{0};
};

template <typename SampleType> struct CepstralEnvelopeResultT {
    SourceFilterAnalysisStatus status = SourceFilterAnalysisStatus::NotPrepared;
    int iterations_performed = 0;

    [[nodiscard]] bool ok() const noexcept {
        return status == SourceFilterAnalysisStatus::Ok;
    }
};

/// Log-magnitude spectrum to liftered log-envelope. Input and output contain
/// exactly `fft_size / 2 + 1` DC-to-Nyquist bins. The caller owns conversion to
/// log magnitude and any linear-domain floor policy.
template <typename SampleType = float> class CepstralEnvelopeAnalyzerT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    using Config = CepstralEnvelopeConfigT<SampleType>;
    using Result = CepstralEnvelopeResultT<SampleType>;

    static bool checked_retained_bytes(const Config& config, std::uint64_t target_max_bytes,
                                       std::uint64_t& bytes) noexcept {
        if (!config_valid_(config))
            return false;
        const auto bins = static_cast<std::uint64_t>(config.fft_size / 2 + 1);
        std::uint64_t fft_bytes = 0;
        CheckedRetainedByteCharge charge(target_max_bytes);
        if (!charge.add<SampleType>(bins) ||
            !charge.add<std::complex<SampleType>>(static_cast<std::uint64_t>(config.fft_size)) ||
            !checked_fft_retained_bytes<SampleType>(static_cast<std::uint64_t>(config.fft_size),
                                                    target_max_bytes, fft_bytes) ||
            !charge.add_retained_bytes(fft_bytes))
            return false;
        bytes = charge.total();
        return true;
    }

    [[nodiscard]] SourceFilterAnalysisStatus prepare(const Config& config) {
        const auto status = config_status_(config);
        if (status != SourceFilterAnalysisStatus::Ok) {
            return status;
        }

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            FftT<SampleType> next_fft(config.fft_size);
            std::vector<SampleType> next_smooth(static_cast<std::size_t>(config.fft_size / 2 + 1),
                                                SampleType{0});
            std::vector<std::complex<SampleType>> next_cepstrum(
                static_cast<std::size_t>(config.fft_size),
                std::complex<SampleType>{SampleType{0}, SampleType{0}});
            config_ = config;
            fft_ = std::move(next_fft);
            smooth_input_ = std::move(next_smooth);
            cepstrum_ = std::move(next_cepstrum);
            prepared_ = true;
            return SourceFilterAnalysisStatus::Ok;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return SourceFilterAnalysisStatus::AllocationFailure;
        } catch (const std::length_error&) {
            return SourceFilterAnalysisStatus::AllocationFailure;
        }
#endif
    }

    [[nodiscard]] bool prepared() const noexcept {
        return prepared_;
    }
    [[nodiscard]] int fft_size() const noexcept {
        return prepared_ ? config_.fft_size : 0;
    }
    [[nodiscard]] int num_bins() const noexcept {
        return prepared_ ? config_.fft_size / 2 + 1 : 0;
    }
    [[nodiscard]] int order() const noexcept {
        return prepared_ ? config_.order : 0;
    }

    /// Rejected input leaves `log_envelope` unchanged. The conservative input
    /// bound proves accepted transforms cannot overflow before output mutation.
    [[nodiscard]] Result estimate(std::span<const SampleType> log_magnitude,
                                  std::span<SampleType> log_envelope) noexcept {
        if (!prepared_)
            return {SourceFilterAnalysisStatus::NotPrepared, 0};
        const auto bins = static_cast<std::size_t>(num_bins());
        if (log_magnitude.size() != bins)
            return {SourceFilterAnalysisStatus::InvalidInputSize, 0};
        if (log_envelope.size() != bins)
            return {SourceFilterAnalysisStatus::InvalidOutputSize, 0};
        for (const auto value : log_magnitude) {
            if (!std::isfinite(value))
                return {SourceFilterAnalysisStatus::NonFiniteInput, 0};
        }
        if (!input_range_valid_(log_magnitude))
            return {SourceFilterAnalysisStatus::NumericalOverflow, 0};

        smooth_(log_magnitude, log_envelope);
        int iterations = 0;
        for (; iterations < config_.true_envelope_iterations; ++iterations) {
            for (std::size_t k = 0; k < bins; ++k)
                smooth_input_[k] = std::max(log_magnitude[k], log_envelope[k]);
            smooth_(smooth_input_, log_envelope);
            if (config_.convergence_tolerance > SampleType{0}) {
                SampleType residual = SampleType{0};
                for (std::size_t k = 0; k < bins; ++k)
                    residual = std::max(residual, log_magnitude[k] - log_envelope[k]);
                if (residual <= config_.convergence_tolerance) {
                    ++iterations;
                    break;
                }
            }
        }
        return {SourceFilterAnalysisStatus::Ok, iterations};
    }

  private:
    static bool config_valid_(const Config& config) noexcept {
        return config_status_(config) == SourceFilterAnalysisStatus::Ok;
    }

    static SourceFilterAnalysisStatus config_status_(const Config& config) noexcept {
        if (!source_filter_fft_size_valid(config.fft_size))
            return SourceFilterAnalysisStatus::InvalidFftSize;
        if (config.order < 0 || config.order > config.fft_size / 2)
            return SourceFilterAnalysisStatus::InvalidOrder;
        if (config.true_envelope_iterations < 0 ||
            config.true_envelope_iterations > kSourceFilterMaximumEnvelopeIterations)
            return SourceFilterAnalysisStatus::InvalidIterationCount;
        if (!std::isfinite(config.convergence_tolerance) ||
            config.convergence_tolerance < SampleType{0})
            return SourceFilterAnalysisStatus::InvalidTolerance;
        return SourceFilterAnalysisStatus::Ok;
    }

    bool input_range_valid_(std::span<const SampleType> log_spectrum) const noexcept {
        SampleType maximum = SampleType{0};
        for (const auto value : log_spectrum)
            maximum = std::max(maximum, std::abs(value));
        // A normalized inverse DFT followed by an unnormalized forward DFT is
        // an orthogonal projection. Its L2 norm cannot grow; each pointwise-max
        // refinement adds at most one original-spectrum norm. The extra
        // N*sqrt(N) and factor four conservatively cover unnormalized butterfly
        // intermediates and complex components without changing accepted data.
        const long double size = static_cast<long double>(config_.fft_size);
        const long double passes = static_cast<long double>(config_.true_envelope_iterations + 2);
        const long double conservative_fft_growth = 4.0L * size * std::sqrt(size) * passes;
        const long double limit = static_cast<long double>(std::numeric_limits<SampleType>::max()) /
                                  conservative_fft_growth;
        return static_cast<long double>(maximum) <= limit;
    }

    void smooth_(std::span<const SampleType> log_spectrum,
                 std::span<SampleType> log_envelope) noexcept {
        const int n = config_.fft_size;
        const int bins = n / 2 + 1;
        const auto* input = log_spectrum.data();
        auto* output = log_envelope.data();
        for (int k = 0; k < bins; ++k)
            cepstrum_[static_cast<std::size_t>(k)] = {input[k], SampleType{0}};
        for (int k = bins; k < n; ++k)
            cepstrum_[static_cast<std::size_t>(k)] = {input[n - k], SampleType{0}};
        fft_.inverse(cepstrum_.data());
        for (int q = config_.order + 1; q < n - config_.order; ++q)
            cepstrum_[static_cast<std::size_t>(q)] = {SampleType{0}, SampleType{0}};
        fft_.forward(cepstrum_.data());
        for (int k = 0; k < bins; ++k)
            output[k] = cepstrum_[static_cast<std::size_t>(k)].real();
    }

    Config config_{};
    FftT<SampleType> fft_{};
    std::vector<SampleType> smooth_input_;
    std::vector<std::complex<SampleType>> cepstrum_;
    bool prepared_ = false;
};

using CepstralEnvelopeConfig = CepstralEnvelopeConfigT<float>;
using CepstralEnvelopeConfig64 = CepstralEnvelopeConfigT<double>;
using CepstralEnvelopeAnalyzer = CepstralEnvelopeAnalyzerT<float>;
using CepstralEnvelopeAnalyzer64 = CepstralEnvelopeAnalyzerT<double>;

template <typename SampleType> struct AutocorrelationResultT {
    SourceFilterAnalysisStatus status = SourceFilterAnalysisStatus::DegenerateInput;
    SampleType input_scale = SampleType{0};

    [[nodiscard]] bool ok() const noexcept {
        return status == SourceFilterAnalysisStatus::Ok;
    }
};

/// Biased autocorrelation of a safely normalized signal. Every lag uses the
/// same `samples.size()` divisor, preserving a positive-semidefinite Toeplitz
/// sequence. The returned values describe `samples / input_scale`.
template <typename SampleType>
[[nodiscard]] AutocorrelationResultT<SampleType>
scaled_autocorrelation(std::span<const SampleType> samples, int order,
                       std::span<SampleType> autocorrelation) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    if (order < 0 || order > kSourceFilterMaximumLpcOrder)
        return {SourceFilterAnalysisStatus::InvalidOrder, SampleType{0}};
    if (samples.size() <= static_cast<std::size_t>(order))
        return {SourceFilterAnalysisStatus::InvalidInputSize, SampleType{0}};
    if (autocorrelation.size() != static_cast<std::size_t>(order + 1))
        return {SourceFilterAnalysisStatus::InvalidOutputSize, SampleType{0}};

    SampleType scale = SampleType{0};
    for (const auto sample : samples) {
        if (!std::isfinite(sample))
            return {SourceFilterAnalysisStatus::NonFiniteInput, SampleType{0}};
        scale = std::max(scale, std::abs(sample));
    }
    if (!(scale > SampleType{0}))
        return {SourceFilterAnalysisStatus::DegenerateInput, SampleType{0}};

    const long double promoted_scale = static_cast<long double>(scale);
    const long double divisor = static_cast<long double>(samples.size());
    for (int lag = 0; lag <= order; ++lag) {
        long double sum = 0.0L;
        const auto count = samples.size() - static_cast<std::size_t>(lag);
        for (std::size_t i = 0; i < count; ++i) {
            const long double left = static_cast<long double>(samples[i]) / promoted_scale;
            const long double right =
                static_cast<long double>(samples[i + static_cast<std::size_t>(lag)]) /
                promoted_scale;
            sum += left * right;
        }
        autocorrelation[static_cast<std::size_t>(lag)] = static_cast<SampleType>(sum / divisor);
    }
    return {SourceFilterAnalysisStatus::Ok, scale};
}

template <typename SampleType> struct LevinsonDurbinResultT {
    SourceFilterAnalysisStatus status = SourceFilterAnalysisStatus::RankDeficient;
    SampleType normalized_prediction_error = SampleType{0};

    [[nodiscard]] bool ok() const noexcept {
        return status == SourceFilterAnalysisStatus::Ok;
    }
};

/// Solves `A(z) = 1 + sum(a[k] z^-(k+1))`. `workspace` must hold `order`
/// elements. Rejection zeroes coefficients and reflections, so partial models
/// are never published.
template <typename SampleType>
[[nodiscard]] LevinsonDurbinResultT<SampleType>
levinson_durbin(std::span<const SampleType> autocorrelation, std::span<SampleType> coefficients,
                std::span<SampleType> reflection_coefficients, std::span<SampleType> workspace,
                SampleType rank_tolerance = std::numeric_limits<SampleType>::epsilon() *
                                            SampleType{64}) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    const auto order = coefficients.size();
    const auto clear_outputs = [&] {
        std::fill(coefficients.begin(), coefficients.end(), SampleType{0});
        std::fill(reflection_coefficients.begin(), reflection_coefficients.end(), SampleType{0});
    };
    clear_outputs();
    if (order == 0 || order > static_cast<std::size_t>(kSourceFilterMaximumLpcOrder) ||
        autocorrelation.size() != order + 1 || reflection_coefficients.size() != order)
        return {SourceFilterAnalysisStatus::InvalidOrder, SampleType{0}};
    if (workspace.size() != order)
        return {SourceFilterAnalysisStatus::InvalidWorkspaceSize, SampleType{0}};
    if (!std::isfinite(rank_tolerance) || rank_tolerance <= SampleType{0} ||
        rank_tolerance >= SampleType{1})
        return {SourceFilterAnalysisStatus::InvalidTolerance, SampleType{0}};
    for (const auto value : autocorrelation) {
        if (!std::isfinite(value))
            return {SourceFilterAnalysisStatus::NonFiniteInput, SampleType{0}};
    }
    if (!(autocorrelation[0] > SampleType{0}))
        return {SourceFilterAnalysisStatus::DegenerateInput, SampleType{0}};

    long double error = static_cast<long double>(autocorrelation[0]);
    const long double tolerance = static_cast<long double>(rank_tolerance);
    const long double minimum_error = error * tolerance;
    const long double sample_max = static_cast<long double>(std::numeric_limits<SampleType>::max());
    const auto assign_checked = [&](long double value, SampleType& destination) {
        if (!std::isfinite(value) || std::abs(value) > sample_max)
            return false;
        destination = static_cast<SampleType>(value);
        return std::isfinite(destination);
    };
    for (std::size_t stage = 0; stage < order; ++stage) {
        long double numerator = static_cast<long double>(autocorrelation[stage + 1]);
        for (std::size_t j = 0; j < stage; ++j)
            numerator += static_cast<long double>(coefficients[j]) *
                         static_cast<long double>(autocorrelation[stage - j]);
        const long double reflection_ld = -numerator / error;
        if (!std::isfinite(reflection_ld)) {
            clear_outputs();
            return {SourceFilterAnalysisStatus::RankDeficient, SampleType{0}};
        }
        if (std::abs(reflection_ld) >= 1.0L - tolerance) {
            const auto status = std::abs(reflection_ld) > 1.0L + tolerance
                                    ? SourceFilterAnalysisStatus::UnstableModel
                                    : SourceFilterAnalysisStatus::RankDeficient;
            clear_outputs();
            return {status, SampleType{0}};
        }

        std::copy(coefficients.begin(), coefficients.end(), workspace.begin());
        for (std::size_t j = 0; j < stage; ++j) {
            const long double updated =
                static_cast<long double>(workspace[j]) +
                reflection_ld * static_cast<long double>(workspace[stage - 1 - j]);
            if (!assign_checked(updated, coefficients[j])) {
                clear_outputs();
                return {SourceFilterAnalysisStatus::NumericalOverflow, SampleType{0}};
            }
        }
        if (!assign_checked(reflection_ld, coefficients[stage]) ||
            !assign_checked(reflection_ld, reflection_coefficients[stage])) {
            clear_outputs();
            return {SourceFilterAnalysisStatus::NumericalOverflow, SampleType{0}};
        }
        error *= 1.0L - reflection_ld * reflection_ld;
        if (!std::isfinite(error) || error <= minimum_error) {
            clear_outputs();
            return {SourceFilterAnalysisStatus::RankDeficient, SampleType{0}};
        }
    }
    if (error > sample_max) {
        clear_outputs();
        return {SourceFilterAnalysisStatus::NumericalOverflow, SampleType{0}};
    }
    return {SourceFilterAnalysisStatus::Ok, static_cast<SampleType>(error)};
}

template <typename SampleType> struct LpcAnalysisResultT {
    SourceFilterAnalysisStatus status = SourceFilterAnalysisStatus::NotPrepared;
    int order = 0;
    SampleType input_scale = SampleType{0};
    SampleType normalized_prediction_error = SampleType{0};
    /// Prediction-error power in the original sample-squared units.
    SampleType prediction_error = SampleType{0};

    [[nodiscard]] bool ok() const noexcept {
        return status == SourceFilterAnalysisStatus::Ok;
    }
};

template <typename SampleType = float> class LpcAnalyzerT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    using Result = LpcAnalysisResultT<SampleType>;

    static bool checked_retained_bytes(int order, std::uint64_t target_max_bytes,
                                       std::uint64_t& bytes) noexcept {
        if (order < 1 || order > kSourceFilterMaximumLpcOrder)
            return false;
        CheckedRetainedByteCharge charge(target_max_bytes);
        if (!charge.add<SampleType>(static_cast<std::uint64_t>(order + 1)) ||
            !charge.add_repeated<SampleType>(static_cast<std::uint64_t>(order), 3))
            return false;
        bytes = charge.total();
        return true;
    }

    [[nodiscard]] SourceFilterAnalysisStatus
    prepare(int order, SampleType rank_tolerance = std::numeric_limits<SampleType>::epsilon() *
                                                   SampleType{64}) {
        if (order < 1 || order > kSourceFilterMaximumLpcOrder) {
            return SourceFilterAnalysisStatus::InvalidOrder;
        }
        if (!std::isfinite(rank_tolerance) || rank_tolerance <= SampleType{0} ||
            rank_tolerance >= SampleType{1}) {
            return SourceFilterAnalysisStatus::InvalidTolerance;
        }

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            std::vector<SampleType> next_autocorrelation(static_cast<std::size_t>(order + 1),
                                                         SampleType{0});
            std::vector<SampleType> next_coefficients(static_cast<std::size_t>(order),
                                                      SampleType{0});
            std::vector<SampleType> next_reflections(static_cast<std::size_t>(order),
                                                     SampleType{0});
            std::vector<SampleType> next_workspace(static_cast<std::size_t>(order), SampleType{0});
            order_ = order;
            rank_tolerance_ = rank_tolerance;
            autocorrelation_ = std::move(next_autocorrelation);
            coefficients_ = std::move(next_coefficients);
            reflections_ = std::move(next_reflections);
            workspace_ = std::move(next_workspace);
            prepared_ = true;
            result_ = {};
            return SourceFilterAnalysisStatus::Ok;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return SourceFilterAnalysisStatus::AllocationFailure;
        } catch (const std::length_error&) {
            return SourceFilterAnalysisStatus::AllocationFailure;
        }
#endif
    }

    /// Failure clears the published coefficient, reflection, and error views.
    [[nodiscard]] Result analyze(std::span<const SampleType> samples) noexcept {
        clear_result_();
        if (!prepared_)
            return result_ = {SourceFilterAnalysisStatus::NotPrepared};
        if (samples.size() <= static_cast<std::size_t>(order_))
            return result_ = {SourceFilterAnalysisStatus::InvalidInputSize};

        const auto autocorrelation_result =
            scaled_autocorrelation<SampleType>(samples, order_, autocorrelation_);
        if (!autocorrelation_result.ok())
            return result_ = {autocorrelation_result.status};
        const auto solver_result = levinson_durbin<SampleType>(
            autocorrelation_, coefficients_, reflections_, workspace_, rank_tolerance_);
        if (!solver_result.ok()) {
            clear_result_();
            return result_ = {solver_result.status};
        }

        const long double scale = static_cast<long double>(autocorrelation_result.input_scale);
        const long double prediction_error =
            static_cast<long double>(solver_result.normalized_prediction_error) * scale * scale;
        if (!std::isfinite(prediction_error) || prediction_error < 0.0L ||
            prediction_error > static_cast<long double>(std::numeric_limits<SampleType>::max())) {
            clear_result_();
            return result_ = {SourceFilterAnalysisStatus::PredictionErrorOverflow};
        }
        result_ = {SourceFilterAnalysisStatus::Ok, order_, autocorrelation_result.input_scale,
                   solver_result.normalized_prediction_error,
                   static_cast<SampleType>(prediction_error)};
        return result_;
    }

    [[nodiscard]] bool prepared() const noexcept {
        return prepared_;
    }
    [[nodiscard]] int order() const noexcept {
        return prepared_ ? order_ : 0;
    }
    [[nodiscard]] Result result() const noexcept {
        return result_;
    }
    [[nodiscard]] std::span<const SampleType> coefficients() const noexcept {
        return result_.ok() ? std::span<const SampleType>(coefficients_)
                            : std::span<const SampleType>{};
    }
    [[nodiscard]] std::span<const SampleType> reflection_coefficients() const noexcept {
        return result_.ok() ? std::span<const SampleType>(reflections_)
                            : std::span<const SampleType>{};
    }

  private:
    void clear_result_() noexcept {
        std::fill(coefficients_.begin(), coefficients_.end(), SampleType{0});
        std::fill(reflections_.begin(), reflections_.end(), SampleType{0});
        result_ = {};
    }

    int order_ = 0;
    SampleType rank_tolerance_ = SampleType{0};
    std::vector<SampleType> autocorrelation_;
    std::vector<SampleType> coefficients_;
    std::vector<SampleType> reflections_;
    std::vector<SampleType> workspace_;
    Result result_{};
    bool prepared_ = false;
};

using LpcAnalyzer = LpcAnalyzerT<float>;
using LpcAnalyzer64 = LpcAnalyzerT<double>;

/// Explicit Schur step-down stability test for the `A(z)` coefficient
/// convention used above. `workspace` must contain exactly `coefficients.size()`
/// elements. `stability_margin` rejects poles closer than that margin to the
/// unit circle.
template <typename SampleType>
[[nodiscard]] SourceFilterAnalysisStatus
lpc_stability(std::span<const SampleType> coefficients, std::span<SampleType> workspace,
              SampleType stability_margin = std::numeric_limits<SampleType>::epsilon() *
                                            SampleType{64}) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    const auto order = coefficients.size();
    if (order == 0 || order > static_cast<std::size_t>(kSourceFilterMaximumLpcOrder))
        return SourceFilterAnalysisStatus::InvalidOrder;
    if (workspace.size() != order)
        return SourceFilterAnalysisStatus::InvalidWorkspaceSize;
    if (!std::isfinite(stability_margin) || stability_margin <= SampleType{0} ||
        stability_margin >= SampleType{1})
        return SourceFilterAnalysisStatus::InvalidTolerance;
    for (const auto coefficient : coefficients) {
        if (!std::isfinite(coefficient))
            return SourceFilterAnalysisStatus::NonFiniteInput;
    }
    std::copy(coefficients.begin(), coefficients.end(), workspace.begin());
    for (std::size_t stage = order; stage > 0; --stage) {
        const long double reflection = static_cast<long double>(workspace[stage - 1]);
        const long double margin = static_cast<long double>(stability_margin);
        if (!std::isfinite(reflection) || std::abs(reflection) >= 1.0L - margin)
            return SourceFilterAnalysisStatus::UnstableModel;
        const long double denominator = 1.0L - reflection * reflection;
        const long double sample_max =
            static_cast<long double>(std::numeric_limits<SampleType>::max());
        const auto assign_checked = [&](long double value, SampleType& destination) {
            if (!std::isfinite(value) || std::abs(value) > sample_max)
                return false;
            destination = static_cast<SampleType>(value);
            return std::isfinite(destination);
        };
        const std::size_t remaining = stage - 1;
        for (std::size_t i = 0; i < remaining / 2; ++i) {
            const std::size_t j = remaining - 1 - i;
            const long double left = static_cast<long double>(workspace[i]);
            const long double right = static_cast<long double>(workspace[j]);
            if (!assign_checked((left - reflection * right) / denominator, workspace[i]) ||
                !assign_checked((right - reflection * left) / denominator, workspace[j]))
                return SourceFilterAnalysisStatus::NumericalOverflow;
        }
        if ((remaining & 1u) != 0u) {
            const std::size_t middle = remaining / 2;
            if (!assign_checked(static_cast<long double>(workspace[middle]) / (1.0L + reflection),
                                workspace[middle]))
                return SourceFilterAnalysisStatus::NumericalOverflow;
        }
    }
    return SourceFilterAnalysisStatus::Ok;
}

/// Evaluates `gain / |A(e^jw)|` at `fft_size / 2 + 1` bins from DC through
/// Nyquist. The explicit Schur check runs before output mutation. This helper
/// is analysis-only and does not construct a runtime filter.
template <typename SampleType>
[[nodiscard]] SourceFilterAnalysisStatus all_pole_magnitude_response(
    std::span<const SampleType> coefficients, SampleType gain, SampleType sample_rate, int fft_size,
    std::span<SampleType> magnitude, std::span<SampleType> stability_workspace,
    SampleType stability_margin = std::numeric_limits<SampleType>::epsilon() * SampleType{64},
    SampleType denominator_floor = std::numeric_limits<SampleType>::epsilon() *
                                   SampleType{64}) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    if (!source_filter_fft_size_valid(fft_size))
        return SourceFilterAnalysisStatus::InvalidFftSize;
    if (!std::isfinite(sample_rate) || sample_rate <= SampleType{0})
        return SourceFilterAnalysisStatus::InvalidSampleRate;
    if (!std::isfinite(gain) || gain < SampleType{0})
        return SourceFilterAnalysisStatus::InvalidGain;
    if (magnitude.size() != static_cast<std::size_t>(fft_size / 2 + 1))
        return SourceFilterAnalysisStatus::InvalidOutputSize;
    if (!std::isfinite(denominator_floor) || denominator_floor <= SampleType{0})
        return SourceFilterAnalysisStatus::InvalidTolerance;
    const auto stability = lpc_stability(coefficients, stability_workspace, stability_margin);
    if (stability != SourceFilterAnalysisStatus::Ok)
        return stability;

    constexpr long double pi = 3.141592653589793238462643383279502884L;
    const auto bins = static_cast<std::size_t>(fft_size / 2 + 1);
    const auto response_value = [&](std::size_t bin) {
        const long double frequency = static_cast<long double>(bin) *
                                      static_cast<long double>(sample_rate) /
                                      static_cast<long double>(fft_size);
        const long double omega = 2.0L * pi * frequency / static_cast<long double>(sample_rate);
        std::complex<long double> denominator{1.0L, 0.0L};
        for (std::size_t k = 0; k < coefficients.size(); ++k)
            denominator += static_cast<long double>(coefficients[k]) *
                           std::polar(1.0L, -omega * static_cast<long double>(k + 1));
        const long double absolute = std::abs(denominator);
        if (!std::isfinite(absolute) || absolute < static_cast<long double>(denominator_floor))
            return std::numeric_limits<long double>::infinity();
        return static_cast<long double>(gain) / absolute;
    };
    for (std::size_t bin = 0; bin < bins; ++bin) {
        const long double value = response_value(bin);
        if (!std::isfinite(value) || value < 0.0L ||
            value > static_cast<long double>(std::numeric_limits<SampleType>::max()))
            return SourceFilterAnalysisStatus::ResponseSingularity;
    }
    for (std::size_t bin = 0; bin < bins; ++bin)
        magnitude[bin] = static_cast<SampleType>(response_value(bin));
    return SourceFilterAnalysisStatus::Ok;
}

} // namespace pulp::signal
