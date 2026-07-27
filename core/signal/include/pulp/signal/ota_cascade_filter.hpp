#pragma once

// Four-pole OTA/ladder-family cascade with a zero-delay nonlinear feedback
// junction.
//
// WHY THIS EXISTS
// Classic IR3109, SSM/CEM-style and transistor-ladder low-pass filters share a
// useful DSP skeleton: four one-pole stages inside one global feedback loop.
// Their audible differences mostly come from control laws and headroom, so this
// header keeps the topology voicing-neutral and leaves measured panel behavior
// to AnalogVcfT.
//
// The feedback junction is implicit because it uses the current sample's
// fourth-stage output. Three fixed Newton iterations solve that loop without an
// audio-thread convergence branch. All state and coefficient math is double,
// including when SampleType is float; low TPT coefficients lose useful
// resolution in float at small cutoff/sample-rate ratios.
//
// Oversampling is a fixed-storage cascade of 2x, 65-tap Kaiser-windowed-sinc
// half-band stages. Configuration and processing allocate no memory. The
// nonlinear junction, pole cascade, tap mix, cross-modulation and output gains
// all run at the top rate so the downsampler removes their images together.

#include <pulp/signal/oversampling_fir.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// Voicing-neutral four-pole nonlinear cascade with fixed-storage oversampling.
///
/// Configuration, reset, and processing are allocation-free and `noexcept`.
/// An instance is suitable for an audio thread, but provides no synchronization;
/// do not mutate it concurrently from another thread. `SampleType` selects the
/// public sample type while internal state and coefficient calculations use
/// `double` precision.
template <typename SampleType = float>
class OtaCascadeFilterT {
public:
    /// Output tap combination selected from the shared four-pole cascade.
    enum class Mode {
        /// Four-pole low-pass response (nominal 24 dB/octave).
        lowpass24,
        /// Two-pole low-pass response (nominal 12 dB/octave).
        lowpass12,
        /// Four-pole high-pass response.
        highpass4,
        /// Four-pole band-pass response.
        bandpass4,
    };

    /// Constructs a 44.1 kHz, 2x, 24 dB/octave low-pass filter.
    ///
    /// Defaults are a 1 kHz pole, zero resonance, a 4.3 feedback ceiling,
    /// unity drive/headroom/output gain, zero bias/compensation/cross-modulation/
    /// drift, and zero smoothing time. Coefficients start snapped to those
    /// targets. Construction allocates no memory.
    OtaCascadeFilterT() noexcept {
        update_smoothing_coefficient();
        update_g_target();
        snap_coefficients();
    }

    /// Sets the base sample rate in Hz and resets all DSP history.
    ///
    /// Values below 64 Hz (including negative values) become 64 Hz so the
    /// mandatory 20 Hz pole floor remains below the Nyquist-derived ceiling.
    /// Recomputes the internal oversampled rate, smoothing, and pole target.
    void set_sample_rate(double sample_rate) noexcept {
        // Keep the mandated 20 Hz pole floor below the 0.45 * sample-rate
        // ceiling even for defensive/invalid host preparation values.
        sample_rate_ = std::max(64.0, sample_rate);
        update_internal_rate();
        update_smoothing_coefficient();
        update_g_target();
        reset();
    }

    /// Sets the oversampling factor to 1, 2, 4, 8, or 16.
    ///
    /// Any other value selects 2x. An effective factor change recomputes the
    /// internal rate and pole target and resets filter, FIR, smoothing, and
    /// drift state. Selecting the already-active effective factor is a no-op.
    void set_oversampling(int factor) noexcept {
        if (!valid_oversampling_factor(factor))
            factor = 2;
        if (factor_ == factor)
            return;
        factor_ = factor;
        update_internal_rate();
        update_g_target();
        reset();
    }

    /// Sets the one-pole corner frequency in Hz.
    ///
    /// The value is clamped to `[20, 0.45 * sample_rate()]` at the base rate.
    /// Updates the smoothed coefficient target without clearing DSP history.
    void set_pole_frequency(double pole_hz) noexcept {
        pole_hz_ = std::clamp(pole_hz, 20.0, 0.45 * sample_rate_);
        update_g_target();
    }

    /// Sets normalized resonance in `[0, 1]`.
    ///
    /// Values outside the range are clamped. The resulting feedback target is
    /// `normalized * k_max`; changing it does not clear DSP history.
    void set_resonance(double normalized) noexcept {
        resonance_ = std::clamp(normalized, 0.0, 1.0);
        update_k_target();
    }

    /// Sets the non-negative maximum feedback coefficient.
    ///
    /// Negative values become zero. The resonance target is recomputed as
    /// `resonance * k_max` without clearing DSP history.
    void set_k_max(double k_max) noexcept {
        k_max_ = std::max(0.0, k_max);
        update_k_target();
    }

    /// Sets input drive in decibels, clamped to `[-96, 72]` dB.
    ///
    /// The corresponding linear gain becomes a smoothed target.
    void set_drive_db(double drive_db) noexcept {
        drive_target_ = std::pow(10.0, std::clamp(drive_db, -96.0, 72.0) / 20.0);
    }

    /// Sets nonlinear-junction headroom, clamped to `[0.25, 16]`.
    ///
    /// The value is dimensionless and becomes a smoothed target.
    void set_saturation_headroom(double headroom) noexcept {
        saturation_target_ = std::clamp(headroom, 0.25, 16.0);
    }

    /// Sets dimensionless asymmetric saturation bias, clamped to `[-2, 2]`.
    ///
    /// The value becomes a smoothed target.
    void set_bias(double bias) noexcept {
        bias_target_ = std::clamp(bias, -2.0, 2.0);
    }

    /// Sets resonance-dependent passband compensation in `[0, 1]`.
    ///
    /// Zero disables compensation and one applies the full feedback-dependent
    /// gain. High-pass mode intentionally remains at unity compensation.
    void set_compensation(double amount) noexcept {
        compensation_ = std::clamp(amount, 0.0, 1.0);
        update_compensation_target();
    }

    /// Sets a non-negative linear output-gain multiplier.
    ///
    /// Negative values become zero. The value becomes a smoothed target.
    void set_output_gain(double gain) noexcept {
        output_gain_target_ = std::max(0.0, gain);
    }

    /// Sets nonlinear output cross-modulation.
    ///
    /// `depth` is clamped to `[-1, 1]`; `drive` is clamped to `[0.1, 16]`
    /// and defaults to 2. Both are dimensionless smoothed targets. A zero
    /// depth disables the effect.
    void set_cross_modulation(double depth, double drive = 2.0) noexcept {
        cross_mod_depth_target_ = std::clamp(depth, -1.0, 1.0);
        cross_mod_drive_target_ = std::clamp(drive, 0.1, 16.0);
    }

    /// Selects the output tap combination without resetting cascade history.
    ///
    /// The compensation target is updated; high-pass mode forces unity
    /// compensation while selected.
    void set_mode(Mode mode) noexcept {
        mode_ = mode;
        update_compensation_target();
    }

    /// Sets the base-rate coefficient-smoothing time in milliseconds.
    ///
    /// Negative values become zero; zero snaps controls to their targets on
    /// the next processed sample. This changes smoothing behavior without
    /// immediately snapping coefficients or clearing DSP history.
    void set_smoothing_time_ms(double milliseconds) noexcept {
        smoothing_ms_ = std::max(0.0, milliseconds);
        update_smoothing_coefficient();
    }

    /// Configures deterministic low-rate cutoff and resonance drift.
    ///
    /// `cutoff_cents` is clamped to `[0, 100]`; `resonance_fraction` to
    /// `[0, 0.1]`; and `cutoff_rate_hz` to `[0.05, 20]` Hz. A positive
    /// `resonance_rate_hz` is clamped to `[0.05, 40]` Hz, while zero or a
    /// negative value makes resonance drift follow the cutoff rate. Both
    /// depths at zero disable drift and restore unity drift multipliers.
    /// Randomness is instance-local and reproducible after `reset()`.
    void set_drift(double cutoff_cents, double resonance_fraction, double cutoff_rate_hz = 1.5,
                   double resonance_rate_hz = 0.0) noexcept {
        drift_cutoff_cents_ = std::clamp(cutoff_cents, 0.0, 100.0);
        drift_resonance_fraction_ = std::clamp(resonance_fraction, 0.0, 0.1);
        drift_cutoff_rate_hz_ = std::clamp(cutoff_rate_hz, 0.05, 20.0);
        drift_resonance_rate_hz_ =
            resonance_rate_hz <= 0.0 ? 0.0 : std::clamp(resonance_rate_hz, 0.05, 40.0);
        if (drift_cutoff_cents_ == 0.0 && drift_resonance_fraction_ == 0.0) {
            drift_g_multiplier_ = 1.0;
            drift_k_multiplier_ = 1.0;
        }
    }

    /// Processes one base-rate sample and returns one base-rate sample.
    ///
    /// Advances smoothing, deterministic drift, FIR oversampling, and cascade
    /// state. The method is allocation-free and real-time safe for an instance
    /// exclusively owned by the calling audio thread.
    SampleType process(SampleType input) noexcept {
        step_base_rate_controls();

        std::array<double, 16> values{};
        std::array<double, 16> expanded{};
        values[0] = static_cast<double>(input);
        std::size_t count = 1;
        const int stages = stage_count();

        for (int stage = 0; stage < stages; ++stage) {
            for (std::size_t i = 0; i < count; ++i) {
                expanded[2 * i] = 2.0 * upsamplers_[stage].process(values[i]);
                expanded[2 * i + 1] = 2.0 * upsamplers_[stage].process(0.0);
            }
            count *= 2;
            std::copy_n(expanded.begin(), count, values.begin());
        }

        for (std::size_t i = 0; i < count; ++i)
            values[i] = process_internal(values[i]);

        for (int stage = stages; stage-- > 0;) {
            count /= 2;
            for (std::size_t i = 0; i < count; ++i) {
                // Emit on the even phase. For a 65-tap round trip this places
                // the symmetric impulse peak exactly at the reported delay.
                const double output = downsamplers_[stage].process(values[2 * i]);
                static_cast<void>(downsamplers_[stage].process(values[2 * i + 1]));
                values[i] = output;
            }
        }

        return static_cast<SampleType>(values[0]);
    }

    /// Processes `num_samples` base-rate samples in place.
    ///
    /// A null `buffer` or non-positive `num_samples` is a no-op. Otherwise this
    /// is equivalent to calling the scalar `process()` for each sample and is
    /// allocation-free.
    void process(SampleType* buffer, int num_samples) noexcept {
        if (buffer == nullptr || num_samples <= 0)
            return;
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    /// Clears cascade and oversampler history while preserving configuration.
    ///
    /// Restores the fixed drift seed and zero drift state, then snaps all
    /// smoothed coefficients to their current targets. The next sample therefore
    /// begins the same deterministic drift sequence for identical settings.
    void reset() noexcept {
        states_.fill(0.0);
        taps_.fill(0.0);
        for (auto& stage : upsamplers_)
            stage.reset();
        for (auto& stage : downsamplers_)
            stage.reset();

        drift_rng_ = kDriftSeed;
        drift_cutoff_state_ = 0.0;
        drift_resonance_state_ = 0.0;
        drift_g_multiplier_ = 1.0;
        drift_k_multiplier_ = 1.0;
        drift_countdown_ = 0;
        snap_coefficients();
    }

    /// Returns round-trip FIR latency for an oversampling factor.
    ///
    /// The result is expressed in base-rate samples: 1x -> 0, 2x -> 32,
    /// 4x -> 48, 8x -> 56, and 16x -> 60. Returns `-1` for any invalid factor.
    static constexpr int latency_samples_for_oversampling(int factor) noexcept {
        if (factor != 1 && factor != 2 && factor != 4 && factor != 8 &&
            factor != 16)
            return -1;
        int latency = 0;
        for (int level = 0; factor > 1; ++level, factor >>= 1)
            latency += 32 >> level;
        return latency;
    }

    /// Returns the current round-trip oversampling latency in base-rate samples.
    int latency_samples() const noexcept {
        return latency_samples_for_oversampling(factor_);
    }

    /// Returns the active oversampling factor: 1, 2, 4, 8, or 16.
    int oversampling() const noexcept {
        return factor_;
    }

    /// Returns the active base sample rate in Hz, after the 64 Hz floor.
    double sample_rate() const noexcept {
        return sample_rate_;
    }

private:
    static constexpr std::uint64_t kDriftSeed = 0xD41F7ull;
    static constexpr double kPi = 3.141592653589793238462643383279502884;

    static bool valid_oversampling_factor(int factor) noexcept {
        return factor == 1 || factor == 2 || factor == 4 || factor == 8 || factor == 16;
    }

    int stage_count() const noexcept {
        int result = 0;
        for (int factor = factor_; factor > 1; factor >>= 1)
            ++result;
        return result;
    }

    static double flush(double value) noexcept {
        return std::abs(value) <= 1.0e-18 ? 0.0 : value;
    }

    static double smooth(double current, double target, double coefficient) noexcept {
        return current + coefficient * (target - current);
    }

    void update_internal_rate() noexcept {
        internal_rate_ = sample_rate_ * static_cast<double>(factor_);
    }

    void update_smoothing_coefficient() noexcept {
        smoothing_coefficient_ =
            smoothing_ms_ <= 0.0 ? 1.0
                                 : 1.0 - std::exp(-1.0 / (smoothing_ms_ * 0.001 * sample_rate_));
    }

    void update_g_target() noexcept {
        const double clamped = std::clamp(pole_hz_, 20.0, 0.45 * sample_rate_);
        const double g = std::tan(kPi * clamped / internal_rate_);
        g_target_ = g / (1.0 + g);
    }

    void update_k_target() noexcept {
        k_target_ = resonance_ * k_max_;
        update_compensation_target();
    }

    void update_compensation_target() noexcept {
        compensation_gain_target_ =
            mode_ == Mode::highpass4 ? 1.0 : 1.0 + compensation_ * k_target_;
    }

    void snap_coefficients() noexcept {
        g_ = g_target_;
        k_ = k_target_;
        drive_ = drive_target_;
        saturation_ = saturation_target_;
        bias_ = bias_target_;
        compensation_gain_ = compensation_gain_target_;
        output_gain_ = output_gain_target_;
        cross_mod_depth_ = cross_mod_depth_target_;
        cross_mod_drive_ = cross_mod_drive_target_;
    }

    std::uint64_t random_u64() noexcept {
        std::uint64_t z = (drift_rng_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    double uniform_open01() noexcept {
        constexpr double kScale = 1.0 / 9007199254740992.0;
        return (static_cast<double>(random_u64() >> 11) + 0.5) * kScale;
    }

    double gaussian() noexcept {
        const double u1 = uniform_open01();
        const double u2 = uniform_open01();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
    }

    void step_drift() noexcept {
        if (drift_cutoff_cents_ == 0.0 && drift_resonance_fraction_ == 0.0) {
            drift_g_multiplier_ = 1.0;
            drift_k_multiplier_ = 1.0;
            return;
        }

        const double dt = 32.0 / sample_rate_;
        const double a1 = std::clamp(2.0 * kPi * drift_cutoff_rate_hz_ * dt, 0.0, 0.5);
        const double resonance_rate =
            drift_resonance_rate_hz_ == 0.0 ? drift_cutoff_rate_hz_ : drift_resonance_rate_hz_;
        const double a2 = std::clamp(2.0 * kPi * resonance_rate * dt, 0.0, 0.5);

        drift_cutoff_state_ += a1 * -drift_cutoff_state_ + std::sqrt(2.0 * a1) * 0.5 * gaussian();
        drift_resonance_state_ +=
            a2 * -drift_resonance_state_ + std::sqrt(2.0 * a2) * 0.5 * gaussian();
        drift_cutoff_state_ = std::clamp(drift_cutoff_state_, -1.5, 1.5);
        drift_resonance_state_ = std::clamp(drift_resonance_state_, -1.5, 1.5);

        drift_g_multiplier_ = std::exp2(drift_cutoff_cents_ * drift_cutoff_state_ / 1200.0);
        drift_k_multiplier_ = 1.0 + drift_resonance_fraction_ * drift_resonance_state_;
    }

    void step_base_rate_controls() noexcept {
        if (drift_countdown_ <= 0) {
            step_drift();
            drift_countdown_ = 32;
        }
        --drift_countdown_;

        const double coefficient = smoothing_coefficient_;
        g_ = smooth(g_, std::clamp(g_target_ * drift_g_multiplier_, 0.0, 0.999999), coefficient);
        k_ = smooth(k_, std::max(0.0, k_target_ * drift_k_multiplier_), coefficient);
        drive_ = smooth(drive_, drive_target_, coefficient);
        saturation_ = smooth(saturation_, saturation_target_, coefficient);
        bias_ = smooth(bias_, bias_target_, coefficient);
        compensation_gain_ = smooth(compensation_gain_, compensation_gain_target_, coefficient);
        output_gain_ = smooth(output_gain_, output_gain_target_, coefficient);
        cross_mod_depth_ = smooth(cross_mod_depth_, cross_mod_depth_target_, coefficient);
        cross_mod_drive_ = smooth(cross_mod_drive_, cross_mod_drive_target_, coefficient);
    }

    double process_internal(double input) noexcept {
        std::array<double, 4> precomputed{};
        for (std::size_t i = 0; i < states_.size(); ++i)
            precomputed[i] = (1.0 - g_) * states_[i];

        const double g2 = g_ * g_;
        const double g3 = g2 * g_;
        const double a = g2 * g2;
        const double b =
            g3 * precomputed[0] + g2 * precomputed[1] + g_ * precomputed[2] + precomputed[3];
        const double driven = drive_ * input;
        const double tanh_bias = std::tanh(bias_);

        double y4 = (a * driven + b) / (1.0 + k_ * a);
        for (int iteration = 0; iteration < 3; ++iteration) {
            const double w = driven - k_ * y4;
            const double th = std::tanh(w / saturation_ + bias_);
            const double f = y4 - a * saturation_ * (th - tanh_bias) - b;
            const double derivative = 1.0 + a * k_ * (1.0 - th * th);
            y4 -= f / derivative;
        }

        const double junction =
            saturation_ * (std::tanh((driven - k_ * y4) / saturation_ + bias_) - tanh_bias);

        double previous = junction;
        for (std::size_t i = 0; i < states_.size(); ++i) {
            const double output = g_ * previous + precomputed[i];
            states_[i] = flush(2.0 * output - states_[i]);
            taps_[i] = output;
            previous = output;
        }

        double output = taps_[3];
        switch (mode_) {
        case Mode::lowpass24:
            output = taps_[3];
            break;
        case Mode::lowpass12:
            output = taps_[1];
            break;
        case Mode::highpass4:
            output = junction - 4.0 * taps_[0] + 6.0 * taps_[1] - 4.0 * taps_[2] + taps_[3];
            break;
        case Mode::bandpass4:
            output = 4.0 * (taps_[1] - 2.0 * taps_[2] + taps_[3]);
            break;
        }

        output *= 1.0 + cross_mod_depth_ * std::tanh(cross_mod_drive_ * driven);
        return output * compensation_gain_ * output_gain_;
    }

    double sample_rate_ = 44100.0;
    double internal_rate_ = 88200.0;
    int factor_ = 2;
    double pole_hz_ = 1000.0;
    double resonance_ = 0.0;
    double k_max_ = 4.3;
    Mode mode_ = Mode::lowpass24;

    double smoothing_ms_ = 0.0;
    double smoothing_coefficient_ = 1.0;

    double g_target_ = 0.0;
    double k_target_ = 0.0;
    double drive_target_ = 1.0;
    double saturation_target_ = 1.0;
    double bias_target_ = 0.0;
    double compensation_ = 0.0;
    double compensation_gain_target_ = 1.0;
    double output_gain_target_ = 1.0;
    double cross_mod_depth_target_ = 0.0;
    double cross_mod_drive_target_ = 2.0;

    double g_ = 0.0;
    double k_ = 0.0;
    double drive_ = 1.0;
    double saturation_ = 1.0;
    double bias_ = 0.0;
    double compensation_gain_ = 1.0;
    double output_gain_ = 1.0;
    double cross_mod_depth_ = 0.0;
    double cross_mod_drive_ = 2.0;

    double drift_cutoff_cents_ = 0.0;
    double drift_resonance_fraction_ = 0.0;
    double drift_cutoff_rate_hz_ = 1.5;
    double drift_resonance_rate_hz_ = 0.0;
    double drift_cutoff_state_ = 0.0;
    double drift_resonance_state_ = 0.0;
    double drift_g_multiplier_ = 1.0;
    double drift_k_multiplier_ = 1.0;
    std::uint64_t drift_rng_ = kDriftSeed;
    int drift_countdown_ = 0;

    std::array<double, 4> states_{};
    std::array<double, 4> taps_{};
    std::array<detail::FixedHalfBandFir65, 4> upsamplers_{};
    std::array<detail::FixedHalfBandFir65, 4> downsamplers_{};
};

/// Single-precision sample interface for `OtaCascadeFilterT<float>`.
using OtaCascadeFilter = OtaCascadeFilterT<float>;
/// Double-precision sample interface for `OtaCascadeFilterT<double>`.
using OtaCascadeFilter64 = OtaCascadeFilterT<double>;

}  // namespace pulp::signal
