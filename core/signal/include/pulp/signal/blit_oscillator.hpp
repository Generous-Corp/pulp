#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <type_traits>

namespace pulp::signal {

/// Band-limited impulse train evaluated by discrete summation.
///
/// With `H = floor(Nyquist / frequency)`, the output is
///
///     (1 + 2 sum(k=1..H, cos(2 pi k phase))) / (2 H + 1)
///
/// evaluated in its closed Dirichlet-kernel form. The normalization gives a
/// unit impulse limit while retaining only DC and the positive/negative partial
/// pairs whose positive member does not exceed Nyquist. The phase path owns
/// fixed scalar state only; prepare, control, reset, and render operations are
/// allocation-free, lock-free, deterministic, and `noexcept`.
template <typename SampleType = float> class BlitOscillatorT {
  public:
    static_assert(std::is_same_v<SampleType, float> || std::is_same_v<SampleType, double>);

    /// Set the sample rate in Hz. Failure leaves the complete prior
    /// configuration unchanged.
    [[nodiscard]] bool prepare(SampleType sample_rate) noexcept {
        return prepare(sample_rate, frequency_hz_);
    }

    /// Atomically set sample rate and fundamental. Use this overload when a
    /// valid pair cannot be reached through either default-backed setter first.
    [[nodiscard]] bool prepare(SampleType sample_rate, SampleType frequency_hz) noexcept {
        if (!valid_configuration(sample_rate, frequency_hz))
            return false;
        sample_rate_ = sample_rate;
        frequency_hz_ = frequency_hz;
        update_harmonics();
        return true;
    }

    /// Set the fundamental in Hz. The open Nyquist bound guarantees at least
    /// one representable positive harmonic. Failure is transactional.
    [[nodiscard]] bool set_frequency(SampleType frequency_hz) noexcept {
        if (!valid_configuration(sample_rate_, frequency_hz))
            return false;
        frequency_hz_ = frequency_hz;
        update_harmonics();
        return true;
    }

    SampleType sample_rate() const noexcept {
        return sample_rate_;
    }
    SampleType frequency() const noexcept {
        return frequency_hz_;
    }
    int harmonic_count() const noexcept {
        return harmonics_;
    }

    /// Set normalized phase in [0, 1). Failure leaves state unchanged.
    [[nodiscard]] bool reset_phase(SampleType phase = SampleType{0}) noexcept {
        if (!std::isfinite(phase) || phase < SampleType{0} || phase >= SampleType{1})
            return false;
        phase_ = static_cast<double>(phase);
        return true;
    }

    double phase() const noexcept {
        return phase_;
    }

    /// Render the current phase and advance one sample.
    SampleType next() noexcept {
        const double distance = std::min(phase_, 1.0 - phase_);
        const double x = std::numbers::pi * distance;
        const double terms = static_cast<double>(2 * harmonics_ + 1);
        const double out = normalized_sinc(terms * x) / normalized_sinc(x);

        phase_ += static_cast<double>(frequency_hz_) / static_cast<double>(sample_rate_);
        phase_ -= std::floor(phase_);
        if (!std::isfinite(phase_))
            phase_ = SampleType{0};
        return std::isfinite(out) ? static_cast<SampleType>(out) : SampleType{0};
    }

  private:
    static constexpr int max_harmonics = (std::numeric_limits<int>::max() - 1) / 2;

    static bool valid_configuration(SampleType sample_rate, SampleType frequency) noexcept {
        if (!std::isfinite(sample_rate) || !(sample_rate > SampleType{0}) ||
            !std::isfinite(frequency) || !(frequency > SampleType{0}) ||
            frequency >= sample_rate * SampleType{0.5})
            return false;
        const long double requested = std::floor((static_cast<long double>(sample_rate) * 0.5L) /
                                                 static_cast<long double>(frequency));
        return std::isfinite(requested) && requested >= 1.0L &&
               requested <= static_cast<long double>(max_harmonics);
    }

    static double normalized_sinc(double x) noexcept {
        const double magnitude = std::fabs(x);
        if (magnitude < 1.0e-4) {
            const double squared = x * x;
            return 1.0 - squared / 6.0 + squared * squared / 120.0;
        }
        return std::sin(x) / x;
    }

    void update_harmonics() noexcept {
        const long double nyquist = static_cast<long double>(sample_rate_) * 0.5L;
        const long double frequency = static_cast<long double>(frequency_hz_);
        harmonics_ = static_cast<int>(std::floor(nyquist / frequency));
        if (static_cast<long double>(harmonics_) * frequency > nyquist)
            --harmonics_;
    }

    SampleType sample_rate_ = SampleType{48000};
    SampleType frequency_hz_ = SampleType{440};
    double phase_ = 0.0;
    int harmonics_ = 54;
};

using BlitOscillator = BlitOscillatorT<float>;
using BlitOscillator64 = BlitOscillatorT<double>;

} // namespace pulp::signal
