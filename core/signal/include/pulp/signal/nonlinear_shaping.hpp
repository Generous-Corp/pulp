#pragma once

/// @file nonlinear_shaping.hpp
/// Alias-controlled wavefolding, harmonic shaping, and ring modulation.

#include <pulp/signal/osc/va.hpp>
#include <pulp/signal/oversampling.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>

namespace pulp::signal {

/// Antialiasing choices shared by the nonlinear-shaping processors.
enum class NonlinearAliasPolicy : std::uint8_t {
    oversample_4x,
    oversample_2x,
    off,
};

namespace detail {

inline constexpr double kNonlinearMinimumSampleRate = 1000.0;
inline constexpr double kNonlinearMaximumSampleRate = 768000.0;

inline bool valid_alias_policy(NonlinearAliasPolicy policy) noexcept {
    switch (policy) {
    case NonlinearAliasPolicy::oversample_4x:
    case NonlinearAliasPolicy::oversample_2x:
    case NonlinearAliasPolicy::off:
        return true;
    }
    return false;
}

template <typename SampleType> class NonlinearOversamplingT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    using Oversampler = OversamplerT<SampleType>;

    bool prepare(double sample_rate) {
        if (!std::isfinite(sample_rate) || sample_rate < kNonlinearMinimumSampleRate ||
            sample_rate > kNonlinearMaximumSampleRate ||
            sample_rate > static_cast<double>(std::numeric_limits<SampleType>::max()))
            return false;
        sample_rate_ = sample_rate;
        oversampler_.set_kind(Oversampler::Kind::linear_phase_fir);
        oversampler_.set_quality(Oversampler::Quality::standard);
        configure_factor();
        oversampler_.set_sample_rate(static_cast<SampleType>(sample_rate_));
        return true;
    }

    bool set_alias_policy(NonlinearAliasPolicy policy) {
        if (!valid_alias_policy(policy) || policy_ == policy)
            return false;
        policy_ = policy;
        configure_factor();
        return true;
    }

    NonlinearAliasPolicy alias_policy() const noexcept {
        return policy_;
    }

    int oversample_factor() const noexcept {
        switch (policy_) {
        case NonlinearAliasPolicy::oversample_4x:
            return 4;
        case NonlinearAliasPolicy::oversample_2x:
            return 2;
        case NonlinearAliasPolicy::off:
            return 1;
        }
        return 1;
    }

    int latency_samples() const noexcept {
        return policy_ == NonlinearAliasPolicy::off ? 0 : oversampler_.latency_samples();
    }

    int tail_samples() const noexcept {
        return 2 * latency_samples();
    }

    double sample_rate() const noexcept {
        return sample_rate_;
    }

    void reset() noexcept {
        oversampler_.reset();
    }

    template <typename Callback> SampleType process(SampleType input, Callback&& callback) {
        if (policy_ == NonlinearAliasPolicy::off)
            return callback(input);
        return oversampler_.process(input, callback);
    }

  private:
    void configure_factor() {
        using Factor = typename Oversampler::Factor;
        oversampler_.set_factor(policy_ == NonlinearAliasPolicy::oversample_4x ? Factor::x4
                                                                               : Factor::x2);
    }

    double sample_rate_ = 48000.0;
    NonlinearAliasPolicy policy_ = NonlinearAliasPolicy::oversample_4x;
    Oversampler oversampler_{};
};

inline double triangle_fold(double input) noexcept {
    constexpr double half_pi = std::numbers::pi / 2.0;
    return (2.0 / std::numbers::pi) * std::asin(std::sin(half_pi * input));
}

inline double triangle_fold_slope(double input) noexcept {
    const double cosine = std::cos((std::numbers::pi / 2.0) * input);
    if (std::abs(cosine) < 1.0e-12)
        return 0.0;
    return cosine > 0.0 ? 1.0 : -1.0;
}

} // namespace detail

/// A sequential wavefolder whose stage drive increases through the chain.
///
/// The default four-times oversampling uses Pulp's standard linear-phase FIR
/// pair and reports its exact host-rate latency. `off` is an explicit raw
/// reference mode, not an antialiased quality setting. Configuration may
/// allocate inside the composed oversampler; `process()`, `process_block()`,
/// and `reset()` allocate nothing and take no locks.
template <typename SampleType = float> class MultistageWavefolderT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    static constexpr int kMaxStages = 8;
    static constexpr double kRawInputLimit = 64.0;

    void prepare(double sample_rate) {
        aliasing_.prepare(sample_rate);
    }

    void set_alias_policy(NonlinearAliasPolicy policy) {
        aliasing_.set_alias_policy(policy);
    }
    NonlinearAliasPolicy alias_policy() const noexcept {
        return aliasing_.alias_policy();
    }
    int oversample_factor() const noexcept {
        return aliasing_.oversample_factor();
    }
    int latency_samples() const noexcept {
        return aliasing_.latency_samples();
    }
    int tail_samples() const noexcept {
        return dc_output() == 0.0 ? aliasing_.tail_samples() : -1;
    }
    double sample_rate() const noexcept {
        return aliasing_.sample_rate();
    }

    void set_stages(int stages) noexcept {
        stages_ = std::clamp(stages, 1, kMaxStages);
    }
    int stages() const noexcept {
        return stages_;
    }

    void set_fold(double fold01) noexcept {
        if (std::isfinite(fold01))
            fold_ = std::clamp(fold01, 0.0, 1.0);
    }
    double fold() const noexcept {
        return fold_;
    }

    void set_stage_offset(int stage, double offset) noexcept {
        if (stage < 0 || stage >= kMaxStages || !std::isfinite(offset))
            return;
        offsets_[static_cast<std::size_t>(stage)] = std::clamp(offset, -1.0, 1.0);
    }
    double stage_offset(int stage) const noexcept {
        if (stage < 0 || stage >= kMaxStages)
            return 0.0;
        return offsets_[static_cast<std::size_t>(stage)];
    }

    void set_stage_symmetry(int stage, double symmetry) noexcept {
        if (stage < 0 || stage >= kMaxStages || !std::isfinite(symmetry))
            return;
        symmetries_[static_cast<std::size_t>(stage)] = std::clamp(symmetry, -1.0, 1.0);
    }
    double stage_symmetry(int stage) const noexcept {
        if (stage < 0 || stage >= kMaxStages)
            return 0.0;
        return symmetries_[static_cast<std::size_t>(stage)];
    }

    /// How much of each stage's zero-input operating point reaches the next
    /// stage. One is a fully DC-coupled cascade; zero subtracts that static
    /// point locally. This is a memoryless transfer-law choice, not a high-pass
    /// filter, so it adds no state, latency, or decay tail.
    void set_dc_coupling(double amount01) noexcept {
        if (!std::isfinite(amount01))
            return;
        dc_couplings_.fill(std::clamp(amount01, 0.0, 1.0));
    }
    void set_stage_dc_coupling(int stage, double amount01) noexcept {
        if (stage < 0 || stage >= kMaxStages || !std::isfinite(amount01))
            return;
        dc_couplings_[static_cast<std::size_t>(stage)] = std::clamp(amount01, 0.0, 1.0);
    }
    double stage_dc_coupling(int stage) const noexcept {
        if (stage < 0 || stage >= kMaxStages)
            return 1.0;
        return dc_couplings_[static_cast<std::size_t>(stage)];
    }

    /// The exact slope at the origin where differentiable; zero at a fold cusp.
    double small_signal_gain() const noexcept {
        double value = 0.0;
        double derivative = 1.0;
        for (int stage = 0; stage < stages_; ++stage) {
            const std::size_t index = static_cast<std::size_t>(stage);
            const double argument =
                stage_gain(stage) * value + offsets_[index] + symmetries_[index] * value * value;
            derivative *= detail::triangle_fold_slope(argument) *
                          (stage_gain(stage) + 2.0 * symmetries_[index] * value);
            value = detail::triangle_fold(argument) -
                    (1.0 - dc_couplings_[index]) * detail::triangle_fold(offsets_[index]);
        }
        return derivative;
    }

    double dc_output() const noexcept {
        return shape(0.0);
    }

    void reset() noexcept {
        aliasing_.reset();
    }

    SampleType process(SampleType input) noexcept {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        const SampleType output = aliasing_.process(input, [this](SampleType sample) {
            return static_cast<SampleType>(shape(static_cast<double>(sample)));
        });
        if (!std::isfinite(static_cast<double>(output))) {
            reset();
            return SampleType{0};
        }
        return output;
    }

    void process_block(const SampleType* input, SampleType* output,
                       std::size_t sample_count) noexcept {
        if (input == nullptr || output == nullptr)
            return;
        for (std::size_t i = 0; i < sample_count; ++i)
            output[i] = process(input[i]);
    }

    double shape(double input) const noexcept {
        double output = std::clamp(input, -kRawInputLimit, kRawInputLimit);
        for (int stage = 0; stage < stages_; ++stage) {
            const std::size_t index = static_cast<std::size_t>(stage);
            const double argument =
                stage_gain(stage) * output + offsets_[index] + symmetries_[index] * output * output;
            output = detail::triangle_fold(argument) -
                     (1.0 - dc_couplings_[index]) * detail::triangle_fold(offsets_[index]);
        }
        return output;
    }

  private:
    double stage_gain(int stage) const noexcept {
        return 1.0 + fold_ * (1.0 + 0.5 * static_cast<double>(stage));
    }

    int stages_ = 3;
    double fold_ = 0.0;
    std::array<double, kMaxStages> offsets_{};
    std::array<double, kMaxStages> symmetries_{};
    std::array<double, kMaxStages> dc_couplings_{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    detail::NonlinearOversamplingT<SampleType> aliasing_{};
};

/// A fixed-storage Chebyshev-series transfer function.
///
/// Coefficient zero is intentionally unavailable: every polynomial is
/// translated by its value at zero so digital silence remains silence even
/// when even harmonics are active. Inputs are constrained to the Chebyshev
/// design domain `[-1, 1]`; the alias policy also band-limits the resulting
/// boundary corners unless raw `off` mode is selected.
template <typename SampleType = float> class ChebyshevHarmonicShaperT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    static constexpr int kMaxHarmonic = 16;

    ChebyshevHarmonicShaperT() noexcept {
        coefficients_[1] = 1.0;
    }

    void prepare(double sample_rate) {
        aliasing_.prepare(sample_rate);
    }
    void set_alias_policy(NonlinearAliasPolicy policy) {
        aliasing_.set_alias_policy(policy);
    }
    NonlinearAliasPolicy alias_policy() const noexcept {
        return aliasing_.alias_policy();
    }
    int oversample_factor() const noexcept {
        return aliasing_.oversample_factor();
    }
    int latency_samples() const noexcept {
        return aliasing_.latency_samples();
    }
    int tail_samples() const noexcept {
        return aliasing_.tail_samples();
    }
    double sample_rate() const noexcept {
        return aliasing_.sample_rate();
    }

    void clear_harmonics() noexcept {
        coefficients_.fill(0.0);
    }

    void set_harmonic(int order, double amplitude) noexcept {
        if (order < 1 || order > kMaxHarmonic || !std::isfinite(amplitude))
            return;
        coefficients_[static_cast<std::size_t>(order)] = std::clamp(amplitude, -1.0, 1.0);
    }

    double harmonic(int order) const noexcept {
        if (order < 1 || order > kMaxHarmonic)
            return 0.0;
        return coefficients_[static_cast<std::size_t>(order)];
    }

    static constexpr double dc_output() noexcept {
        return 0.0;
    }

    /// The exact derivative of the configured, unclipped series at zero.
    double small_signal_gain() const noexcept {
        double gain = 0.0;
        for (int order = 1; order <= kMaxHarmonic; ++order) {
            const int residue = order & 3;
            const double derivative = residue == 1   ? static_cast<double>(order)
                                      : residue == 3 ? -static_cast<double>(order)
                                                     : 0.0;
            gain += coefficients_[static_cast<std::size_t>(order)] * derivative;
        }
        return gain;
    }

    /// A transfer-domain bound, independent of the selected alias filter.
    double worst_case_output() const noexcept {
        double bound = 0.0;
        for (int order = 1; order <= kMaxHarmonic; ++order)
            bound += std::abs(coefficients_[static_cast<std::size_t>(order)]) *
                     ((order & 1) == 0 ? 2.0 : 1.0);
        return bound;
    }

    void reset() noexcept {
        aliasing_.reset();
    }

    SampleType process(SampleType input) noexcept {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        const SampleType output = aliasing_.process(input, [this](SampleType sample) {
            return static_cast<SampleType>(shape(static_cast<double>(sample)));
        });
        if (!std::isfinite(static_cast<double>(output))) {
            reset();
            return SampleType{0};
        }
        return output;
    }

    void process_block(const SampleType* input, SampleType* output,
                       std::size_t sample_count) noexcept {
        if (input == nullptr || output == nullptr)
            return;
        for (std::size_t i = 0; i < sample_count; ++i)
            output[i] = process(input[i]);
    }

    double shape(double input) const noexcept {
        const double x = std::clamp(input, -1.0, 1.0);
        double previous = 1.0;
        double current = x;
        double output = coefficients_[1] * current;
        for (int order = 2; order <= kMaxHarmonic; ++order) {
            const double next = 2.0 * x * current - previous;
            const double at_zero = (order & 1) != 0 ? 0.0 : ((order & 3) == 0 ? 1.0 : -1.0);
            output += coefficients_[static_cast<std::size_t>(order)] * (next - at_zero);
            previous = current;
            current = next;
        }
        return output;
    }

  private:
    std::array<double, kMaxHarmonic + 1> coefficients_{};
    detail::NonlinearOversamplingT<SampleType> aliasing_{};
};

enum class RingModulationModel : std::uint8_t {
    ideal_multiplier,
    diode_ring,
};

enum class RingCarrierWaveform : std::uint8_t {
    sine,
    triangle,
    square,
};

enum class RingCarrierMode : std::uint8_t {
    bipolar,
    unipolar,
};

enum class RingOutputPolarity : std::uint8_t {
    normal,
    inverted,
};

/// An internally carried ring modulator with ideal and diode-ring models.
///
/// Keeping the carrier inside the oversampled callback makes the carrier and
/// audio meet at the same rate; multiplying a host-rate carrier after the
/// upsampler would retain the very sum sideband the policy is meant to reject.
template <typename SampleType = float> class NonlinearRingModulatorT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    NonlinearRingModulatorT() noexcept {
        carrier_.set_shape(osc::VaShape::sine);
    }

    void prepare(double sample_rate) {
        if (!aliasing_.prepare(sample_rate))
            return;
        sample_rate_ = aliasing_.sample_rate();
        carrier_hz_ = std::clamp(carrier_hz_, 0.0, 0.49 * sample_rate_);
        update_phase_increment();
        reset_carrier();
    }

    void set_alias_policy(NonlinearAliasPolicy policy) {
        if (!aliasing_.set_alias_policy(policy))
            return;
        update_phase_increment();
        reset_carrier();
    }
    NonlinearAliasPolicy alias_policy() const noexcept {
        return aliasing_.alias_policy();
    }
    int oversample_factor() const noexcept {
        return aliasing_.oversample_factor();
    }
    int latency_samples() const noexcept {
        return aliasing_.latency_samples();
    }
    int tail_samples() const noexcept {
        return aliasing_.tail_samples();
    }
    double sample_rate() const noexcept {
        return sample_rate_;
    }

    void set_model(RingModulationModel model) noexcept {
        switch (model) {
        case RingModulationModel::ideal_multiplier:
        case RingModulationModel::diode_ring:
            model_ = model;
            return;
        }
    }
    RingModulationModel model() const noexcept {
        return model_;
    }

    void set_carrier_hz(double hz) noexcept {
        if (!std::isfinite(hz))
            return;
        const double current_phase = phase();
        carrier_hz_ = std::clamp(hz, 0.0, 0.49 * sample_rate_);
        update_phase_increment();
        reset_carrier(current_phase);
    }
    double carrier_hz() const noexcept {
        return carrier_hz_;
    }

    void set_index(double index01) noexcept {
        if (std::isfinite(index01))
            index_ = std::clamp(index01, 0.0, 1.0);
    }
    double index() const noexcept {
        return index_;
    }

    void set_nonlinear_drive(double drive) noexcept {
        if (std::isfinite(drive))
            nonlinear_drive_ = std::clamp(drive, 0.25, 16.0);
    }
    double nonlinear_drive() const noexcept {
        return nonlinear_drive_;
    }

    void set_carrier_waveform(RingCarrierWaveform waveform) noexcept {
        if (waveform_ == waveform)
            return;
        osc::VaShape shape;
        switch (waveform) {
        case RingCarrierWaveform::sine:
            shape = osc::VaShape::sine;
            break;
        case RingCarrierWaveform::triangle:
            shape = osc::VaShape::triangle;
            break;
        case RingCarrierWaveform::square:
            shape = osc::VaShape::square;
            break;
        default:
            return;
        }
        const double current_phase = phase();
        carrier_.set_shape(shape);
        reset_carrier(current_phase);
        waveform_ = waveform;
    }
    RingCarrierWaveform carrier_waveform() const noexcept {
        return waveform_;
    }

    void set_carrier_mode(RingCarrierMode mode) noexcept {
        switch (mode) {
        case RingCarrierMode::bipolar:
        case RingCarrierMode::unipolar:
            carrier_mode_ = mode;
            return;
        }
    }
    RingCarrierMode carrier_mode() const noexcept {
        return carrier_mode_;
    }

    void set_output_polarity(RingOutputPolarity polarity) noexcept {
        switch (polarity) {
        case RingOutputPolarity::normal:
        case RingOutputPolarity::inverted:
            output_polarity_ = polarity;
            return;
        }
    }
    RingOutputPolarity output_polarity() const noexcept {
        return output_polarity_;
    }

    void set_phase(double cycles) noexcept {
        if (!std::isfinite(cycles))
            return;
        reset_carrier(cycles);
    }
    double phase() const noexcept {
        const double value = carrier_.phase() + carrier_delay_cycles();
        return value - std::floor(value);
    }

    static constexpr double dc_output() noexcept {
        return 0.0;
    }

    void reset() noexcept {
        aliasing_.reset();
        reset_carrier();
    }

    SampleType process(SampleType input) noexcept {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        const SampleType output = aliasing_.process(input, [this](SampleType sample) {
            const double waveform = carrier_.next(phase_increment_);
            const double mapped_carrier =
                carrier_mode_ == RingCarrierMode::bipolar ? waveform : 0.5 * (waveform + 1.0);
            return static_cast<SampleType>(shape(static_cast<double>(sample), mapped_carrier));
        });
        if (!std::isfinite(static_cast<double>(output))) {
            reset();
            return SampleType{0};
        }
        return output;
    }

    void process_block(const SampleType* input, SampleType* output,
                       std::size_t sample_count) noexcept {
        if (input == nullptr || output == nullptr)
            return;
        for (std::size_t i = 0; i < sample_count; ++i)
            output[i] = process(input[i]);
    }

    double shape(double input, double carrier) const noexcept {
        carrier = std::clamp(carrier, -1.0, 1.0);
        double modulated = 0.0;
        switch (model_) {
        case RingModulationModel::ideal_multiplier:
            modulated = input * carrier;
            break;
        case RingModulationModel::diode_ring: {
            const double drive = nonlinear_drive_;
            const double half_input = 0.5 * input;
            const double plus = carrier + half_input;
            const double minus = carrier - half_input;
            const double magnitude_difference = stable_magnitude_difference(half_input, carrier);
            modulated = magnitude_difference + (log_cosh_correction(drive, std::abs(plus)) -
                                                log_cosh_correction(drive, std::abs(minus))) /
                                                   drive;
            break;
        }
        }
        const double mixed = std::lerp(input, modulated, index_);
        return output_polarity_ == RingOutputPolarity::normal ? mixed : -mixed;
    }

  private:
    double carrier_delay_cycles() const noexcept {
        return 0.5 * static_cast<double>(aliasing_.latency_samples()) * carrier_hz_ / sample_rate_;
    }

    void reset_carrier(double logical_phase = 0.0) noexcept {
        carrier_.reset(logical_phase - carrier_delay_cycles());
    }

    void update_phase_increment() noexcept {
        phase_increment_ =
            carrier_hz_ / (sample_rate_ * static_cast<double>(aliasing_.oversample_factor()));
    }

    static double stable_magnitude_difference(double half_input, double carrier) noexcept {
        const double carrier_magnitude = std::abs(carrier);
        const double limited = std::clamp(half_input, -carrier_magnitude, carrier_magnitude);
        if (carrier > 0.0)
            return 2.0 * limited;
        if (carrier < 0.0)
            return -2.0 * limited;
        return 0.0;
    }

    static double log_cosh_correction(double drive, double magnitude) noexcept {
        constexpr double kNegligibleCorrectionArgument = 40.0;
        if (magnitude >= kNegligibleCorrectionArgument / drive)
            return 0.0;
        return std::log1p(std::exp(-2.0 * drive * magnitude));
    }

    double sample_rate_ = 48000.0;
    double carrier_hz_ = 440.0;
    double phase_increment_ = 440.0 / (48000.0 * 4.0);
    double index_ = 1.0;
    double nonlinear_drive_ = 2.0;
    RingModulationModel model_ = RingModulationModel::diode_ring;
    RingCarrierWaveform waveform_ = RingCarrierWaveform::sine;
    RingCarrierMode carrier_mode_ = RingCarrierMode::bipolar;
    RingOutputPolarity output_polarity_ = RingOutputPolarity::normal;
    osc::VaOscillator carrier_{};
    detail::NonlinearOversamplingT<SampleType> aliasing_{};
};

using MultistageWavefolder = MultistageWavefolderT<float>;
using MultistageWavefolder64 = MultistageWavefolderT<double>;
using ChebyshevHarmonicShaper = ChebyshevHarmonicShaperT<float>;
using ChebyshevHarmonicShaper64 = ChebyshevHarmonicShaperT<double>;
using NonlinearRingModulator = NonlinearRingModulatorT<float>;
using NonlinearRingModulator64 = NonlinearRingModulatorT<double>;

} // namespace pulp::signal
