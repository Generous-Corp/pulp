#pragma once

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
    static_assert(std::is_floating_point_v<SampleType>);

    /// Set the sample rate in Hz. Failure leaves the complete prior
    /// configuration unchanged.
    [[nodiscard]] bool prepare(SampleType sample_rate) noexcept {
        if (!std::isfinite(sample_rate) || !(sample_rate > SampleType{0}))
            return false;
        if (!(frequency_hz_ > SampleType{0}) || frequency_hz_ >= sample_rate * SampleType{0.5})
            return false;
        sample_rate_ = sample_rate;
        update_harmonics();
        return true;
    }

    /// Set the fundamental in Hz. The open Nyquist bound guarantees at least
    /// one representable positive harmonic. Failure is transactional.
    [[nodiscard]] bool set_frequency(SampleType frequency_hz) noexcept {
        if (!std::isfinite(frequency_hz) || !(frequency_hz > SampleType{0}) ||
            frequency_hz >= sample_rate_ * SampleType{0.5})
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
        phase_ = phase;
        return true;
    }

    SampleType phase() const noexcept {
        return phase_;
    }

    /// Render the current phase and advance one sample.
    SampleType next() noexcept {
        const SampleType x = std::numbers::pi_v<SampleType> * phase_;
        const SampleType denominator = std::sin(x);
        const SampleType terms = static_cast<SampleType>(2 * harmonics_ + 1);

        SampleType out = SampleType{1};
        if (std::fabs(denominator) > SampleType{16} * std::numeric_limits<SampleType>::epsilon())
            out = std::sin(terms * x) / (terms * denominator);

        phase_ += frequency_hz_ / sample_rate_;
        phase_ -= std::floor(phase_);
        if (!std::isfinite(phase_))
            phase_ = SampleType{0};
        return std::isfinite(out) ? out : SampleType{0};
    }

  private:
    void update_harmonics() noexcept {
        const SampleType positive = std::floor((sample_rate_ * SampleType{0.5}) / frequency_hz_);
        if (!std::isfinite(positive) || positive < SampleType{1}) {
            harmonics_ = 1;
            return;
        }
        constexpr int max_harmonics = (std::numeric_limits<int>::max() - 1) / 2;
        const SampleType max_int = static_cast<SampleType>(max_harmonics);
        harmonics_ = positive >= max_int ? max_harmonics : static_cast<int>(positive);
    }

    SampleType sample_rate_ = SampleType{48000};
    SampleType frequency_hz_ = SampleType{440};
    SampleType phase_ = SampleType{0};
    int harmonics_ = 54;
};

using BlitOscillator = BlitOscillatorT<float>;
using BlitOscillator64 = BlitOscillatorT<double>;

} // namespace pulp::signal
