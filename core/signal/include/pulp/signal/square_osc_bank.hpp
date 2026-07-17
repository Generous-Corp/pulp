#pragma once

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/oscillator.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

/// A bank of band-limited square-wave oscillators at fixed frequencies, summed
/// into one output.
///
/// This is the shared tone source for the metallic 808 voices. The cymbal, open
/// hat and closed hat are all the same six-oscillator inharmonic cluster into a
/// filter; the cowbell is two oscillators into a bandpass. Those voices differ
/// only in their filtering and amplitude envelope, not in the oscillator bank
/// itself, so the bank lives here once and each voice is a thin wrapper.
///
/// The frequencies are deliberately inharmonic (not integer multiples of a
/// fundamental): a metallic timbre is a dense, clangorous cluster of partials
/// with no shared period, which is exactly what a handful of detuned squares
/// summed together produce. A single square already carries only odd harmonics;
/// six of them at incommensurate pitches fill the spectrum without ever landing
/// on a tidy harmonic series, and that is the "808 metal" sound.
///
/// Each oscillator is the repository's band-limited OscillatorT set to its
/// square waveform, so the sum inherits that class's polyBLEP anti-aliasing
/// rather than a naive sign() square's alias skirt. See the anti-alias
/// assertions in test_square_osc_bank.cpp for the measured alias floor.
///
/// RT contract: prepare() and set_partials() size the internal storage and may
/// allocate. Every per-sample and per-parameter call after that -- process(),
/// reset(), set_frequency(), set_amplitude(), set_level() -- allocates nothing
/// and never resizes.
template <typename SampleType = float>
class SquareOscBankT {
public:
    /// One partial of the bank: a frequency in Hz and a linear weight. The
    /// weight is applied to that oscillator's +/-1 square before summing, so
    /// the summed output is bounded by set_level() times the sum of the
    /// absolute weights.
    struct Partial {
        SampleType frequency = SampleType{0};
        SampleType amplitude = SampleType{1};
    };

    /// Set the sample rate and re-arm the existing oscillators for it. Safe to
    /// call before or after set_partials(); does not change the partial layout.
    void prepare(SampleType sample_rate) {
        sample_rate_ = sample_rate > SampleType{0} ? sample_rate : SampleType{44100};
        for (auto& osc : osc_) osc.set_sample_rate(sample_rate_);
        reset();
    }

    /// Install the bank's partials. Allocates when the count changes; this is a
    /// prepare-time call, not an audio-thread call. Phases are reset so the
    /// cluster always starts coherent.
    void set_partials(const Partial* partials, std::size_t count) {
        using Wave = typename OscillatorT<SampleType>::Waveform;
        osc_.resize(count);
        amplitude_.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            osc_[i].set_sample_rate(sample_rate_);
            osc_[i].set_waveform(Wave::square);
            osc_[i].set_frequency(partials[i].frequency);
            amplitude_[i] = partials[i].amplitude;
        }
        reset();
    }

    /// Convenience overload for a std::vector of partials.
    void set_partials(const std::vector<Partial>& partials) {
        set_partials(partials.data(), partials.size());
    }

    std::size_t size() const noexcept { return osc_.size(); }

    /// Retune one oscillator in place. No allocation; out-of-range is a no-op.
    void set_frequency(std::size_t index, SampleType hz) noexcept {
        if (index < osc_.size()) osc_[index].set_frequency(hz);
    }

    /// Reweight one oscillator in place. No allocation; out-of-range is a no-op.
    void set_amplitude(std::size_t index, SampleType amp) noexcept {
        if (index < amplitude_.size()) amplitude_[index] = amp;
    }

    SampleType frequency(std::size_t index) const noexcept {
        return index < osc_.size() ? osc_[index].frequency() : SampleType{0};
    }

    SampleType amplitude(std::size_t index) const noexcept {
        return index < amplitude_.size() ? amplitude_[index] : SampleType{0};
    }

    /// Overall output gain applied after the weighted sum.
    void set_level(SampleType level) noexcept { level_ = level; }
    SampleType level() const noexcept { return level_; }

    /// Zero every oscillator's phase so the cluster restarts coherent. Cheap;
    /// safe on the audio thread. Not called implicitly by process().
    void reset() noexcept {
        for (auto& osc : osc_) osc.reset();
    }

    /// Sum of the weighted band-limited squares for the next sample. No
    /// allocation, no locking, no resizing.
    SampleType process() noexcept {
        SampleType sum = SampleType{0};
        const std::size_t n = osc_.size();
        for (std::size_t i = 0; i < n; ++i) {
            sum += amplitude_[i] * osc_[i].next();
        }
        return snap_to_zero(level_ * sum);
    }

    /// The largest magnitude process() can return with the current partials and
    /// level: |level| times the sum of the absolute weights. A caller can use
    /// this to normalize the bank to unity, or a test can assert the output
    /// never exceeds it.
    SampleType peak_bound() const noexcept {
        SampleType s = SampleType{0};
        for (SampleType a : amplitude_) s += std::abs(a);
        return std::abs(level_) * s;
    }

private:
    SampleType sample_rate_ = SampleType{44100};
    SampleType level_ = SampleType{1};
    std::vector<OscillatorT<SampleType>> osc_;
    std::vector<SampleType> amplitude_;
};

using SquareOscBank = SquareOscBankT<float>;
using SquareOscBank64 = SquareOscBankT<double>;

} // namespace pulp::signal
