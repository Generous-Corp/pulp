#pragma once

/// @file ballistics_filter.hpp
/// Peak/RMS envelope follower with independent attack and release curves.

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::signal {

/// Envelope follower with configurable attack and release times.
///
/// RT contract: `process()`, `current()`, and `reset()` allocate no memory.
/// `prepare()` and parameter setters recompute coefficients and should be run
/// outside the audio callback unless the caller owns the retiming point.
///
/// Tracks the envelope of an input signal using first-order IIR
/// smoothing with separate attack (rising) and release (falling)
/// time constants.
///
/// @code
/// BallisticsFilter env;
/// env.prepare(44100.0f);
/// env.set_attack_ms(1.0f);
/// env.set_release_ms(100.0f);
/// float envelope = env.process(std::abs(sample));
/// @endcode
template <typename SampleType = float> class BallisticsFilterT {
  public:
    enum class Mode { peak, rms };
    enum class TimeConvention { legacy_nominal, exact_10_to_90 };

    BallisticsFilterT() = default;

    explicit BallisticsFilterT(TimeConvention time_convention)
        : time_convention_(time_convention) {}

    void prepare(SampleType sample_rate) {
        sample_rate_ = sample_rate;
        update_coefficients();
    }

    void set_attack_ms(SampleType ms) {
        if (!std::isfinite(static_cast<double>(ms)))
            return;
        attack_ms_ = std::max(SampleType{0.01f}, ms);
        update_coefficients();
    }

    void set_release_ms(SampleType ms) {
        if (!std::isfinite(static_cast<double>(ms)))
            return;
        release_ms_ = std::max(SampleType{0.01f}, ms);
        update_coefficients();
    }

    void set_mode(Mode m) {
        mode_ = m;
    }

    SampleType process(SampleType input) {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        if (!std::isfinite(static_cast<double>(state_)) || state_ < SampleType{0})
            reset();

        const SampleType magnitude = std::abs(input);
        SampleType value = magnitude;
        if (mode_ == Mode::rms) {
            const SampleType square_limit = std::sqrt(std::numeric_limits<SampleType>::max());
            value = magnitude > square_limit ? std::numeric_limits<SampleType>::max()
                                             : magnitude * magnitude;
        }
        SampleType coeff = (value > state_) ? attack_coeff_ : release_coeff_;
        // Snap the envelope state so a long release into silence flushes to
        // zero instead of stalling in the denormal range. No-op above 1e-15.
        state_ = snap_to_zero(state_ + coeff * (value - state_));
        if (!std::isfinite(static_cast<double>(state_)) || state_ < SampleType{0}) {
            reset();
            return SampleType{0};
        }
        return (mode_ == Mode::rms) ? std::sqrt(state_) : state_;
    }

    void process(const SampleType* input, SampleType* output, int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            output[i] = process(input[i]);
        }
    }

    SampleType current() const {
        if (!std::isfinite(static_cast<double>(state_)) || state_ < SampleType{0})
            return SampleType{0};
        return (mode_ == Mode::rms) ? std::sqrt(state_) : state_;
    }

    /// Current envelope in dBFS, floored at @p floor_db.
    ///
    /// A non-finite or positive floor is malformed and uses -160 dBFS. The
    /// return value is always finite, including after hostile detector input.
    SampleType current_db(SampleType floor_db = SampleType{-160}) const {
        if (!std::isfinite(static_cast<double>(floor_db)) || floor_db > SampleType{0})
            floor_db = SampleType{-160};
        const SampleType value = current();
        if (!(value > SampleType{0}))
            return floor_db;
        const SampleType db = SampleType{20} * std::log10(value);
        return std::isfinite(static_cast<double>(db)) ? std::max(floor_db, db) : floor_db;
    }

    SampleType attack_ms() const noexcept {
        return attack_ms_;
    }
    SampleType release_ms() const noexcept {
        return release_ms_;
    }
    SampleType attack_coefficient() const noexcept {
        return attack_coeff_;
    }
    SampleType release_coefficient() const noexcept {
        return release_coeff_;
    }

    /// Legacy nominal coefficient retained for bit-identical BallisticsFilter output.
    ///
    /// The historical 2.2 exponent approximates, but does not exactly equal,
    /// a 10-to-90-percent interval. New code needing the exact contract should
    /// use `EnvelopeFollowerT` from `dynamics_contract.hpp`.
    static SampleType coefficient_for_time_ms(SampleType ms, SampleType sample_rate) {
        if (!std::isfinite(static_cast<double>(ms)) ||
            !std::isfinite(static_cast<double>(sample_rate)) || !(sample_rate > SampleType{0}))
            return SampleType{0};
        if (!(ms > SampleType{0.01f}))
            return SampleType{1};
        return SampleType{1} -
               std::exp(SampleType{-2.2f} / (ms * SampleType{0.001f} * sample_rate));
    }

    /// Pure coefficient for an exact 10-to-90-percent state-domain interval.
    static SampleType exact_coefficient_for_time_ms(SampleType ms, SampleType sample_rate) {
        if (!std::isfinite(static_cast<double>(ms)) ||
            !std::isfinite(static_cast<double>(sample_rate)) || !(sample_rate > SampleType{0}))
            return SampleType{0};
        if (!(ms > SampleType{0.01f}))
            return SampleType{1};
        return SampleType{1} -
               std::exp(-std::log(SampleType{9}) / (ms * SampleType{0.001} * sample_rate));
    }

    void reset() {
        state_ = SampleType{0.0f};
    }

  private:
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType attack_ms_ = SampleType{1.0f};
    SampleType release_ms_ = SampleType{100.0f};
    SampleType attack_coeff_ = SampleType{0.0f};
    SampleType release_coeff_ = SampleType{0.0f};
    SampleType state_ = SampleType{0.0f};
    Mode mode_ = Mode::peak;
    TimeConvention time_convention_ = TimeConvention::legacy_nominal;

    void update_coefficients() {
        if (sample_rate_ <= 0)
            return;
        attack_coeff_ = time_constant(attack_ms_);
        release_coeff_ = time_constant(release_ms_);
    }

    SampleType time_constant(SampleType ms) const {
        return time_convention_ == TimeConvention::exact_10_to_90
                   ? exact_coefficient_for_time_ms(ms, sample_rate_)
                   : coefficient_for_time_ms(ms, sample_rate_);
    }
};

using BallisticsFilter = BallisticsFilterT<float>;
using BallisticsFilter64 = BallisticsFilterT<double>;

} // namespace pulp::signal
